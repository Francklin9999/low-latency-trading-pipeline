#define _GNU_SOURCE

#include "afxdp_receiver.h"

#include <errno.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/libxdp.h>
#include <xdp/xsk.h>

#define AFXDP_NUM_FRAMES 4096u
#define AFXDP_FRAME_SIZE XSK_UMEM__DEFAULT_FRAME_SIZE
#define INVALID_UMEM_FRAME UINT64_MAX

struct afxdp_ctx {
    struct xsk_ring_prod fq;
    struct xsk_ring_cons cq;
    struct xsk_ring_cons rx;
    struct xsk_ring_prod tx;

    struct xsk_umem *umem;
    struct xsk_socket *xsk;
    struct xdp_program *xdp_prog;
    int xsks_map_fd;
    int ifindex;

    void *buffer;

    uint64_t umem_frame_addr[AFXDP_NUM_FRAMES];
    uint32_t umem_frame_free;
};

#ifndef XDP_FILTER_OBJ
#define XDP_FILTER_OBJ "afxdp/xdp_filter.bpf.o"
#endif

static uint64_t xsk_alloc_umem_frame(struct afxdp_ctx *ctx)
{
    if (ctx->umem_frame_free == 0)
        return INVALID_UMEM_FRAME;

    return ctx->umem_frame_addr[--ctx->umem_frame_free];
}

static int xsk_free_umem_frame(struct afxdp_ctx *ctx, uint64_t frame)
{
    if (ctx->umem_frame_free >= AFXDP_NUM_FRAMES) {
        errno = ENOBUFS;
        return -1;
    }

    ctx->umem_frame_addr[ctx->umem_frame_free++] = frame;
    return 0;
}

static int afxdp_refill_fq(struct afxdp_ctx *ctx)
{
    uint32_t idx_fq = 0;
    uint32_t free_frames;
    uint32_t free_slots;
    uint32_t to_fill;
    int ret;

    free_frames = ctx->umem_frame_free;
    if (!free_frames)
        return 0;

    free_slots = xsk_prod_nb_free(&ctx->fq, free_frames);
    to_fill = free_slots < free_frames ? free_slots : free_frames;
    if (!to_fill)
        return 0;

    ret = xsk_ring_prod__reserve(&ctx->fq, to_fill, &idx_fq);
    if (ret < 0) {
        errno = -ret;
        return -1;
    }
    if (ret == 0)
        return 0;

    for (int i = 0; i < ret; i++)
        *xsk_ring_prod__fill_addr(&ctx->fq, idx_fq++) = xsk_alloc_umem_frame(ctx);

    xsk_ring_prod__submit(&ctx->fq, ret);

    if (xsk_ring_prod__needs_wakeup(&ctx->fq))
        (void)recvfrom(xsk_socket__fd(ctx->xsk), NULL, 0, MSG_DONTWAIT, NULL, NULL);

    return ret;
}

static int afxdp_load_xdp_filter(struct afxdp_ctx *ctx,
                                  int ifindex,
                                  uint32_t xdp_flags)
{
    struct xdp_program *prog;
    struct bpf_object *obj;
    struct bpf_map *map;
    enum xdp_attach_mode mode;
    int ret;

    prog = xdp_program__open_file(XDP_FILTER_OBJ, "xdp", NULL);
    if (!prog) {
        fprintf(stderr, "[afxdp] could not open %s: %s (using default catch-all)\n",
                XDP_FILTER_OBJ, strerror(errno));
        return -1;
    }

    mode = (xdp_flags & XDP_FLAGS_SKB_MODE) ? XDP_MODE_SKB : XDP_MODE_NATIVE;
    ret = xdp_program__attach(prog, ifindex, mode, 0);
    if (ret) {
        fprintf(stderr, "[afxdp] xdp_program__attach failed: %s\n", strerror(-ret));
        xdp_program__close(prog);
        return -1;
    }

    obj = xdp_program__bpf_obj(prog);
    map = bpf_object__find_map_by_name(obj, "xsks_map");
    if (!map) {
        fprintf(stderr, "[afxdp] xsks_map not found in BPF object\n");
        xdp_program__detach(prog, ifindex, mode, 0);
        xdp_program__close(prog);
        return -1;
    }

    ctx->xdp_prog = prog;
    ctx->xsks_map_fd = bpf_map__fd(map);
    ctx->ifindex = ifindex;

    printf("[afxdp] loaded custom XDP filter from %s (UDP port %d only)\n",
           XDP_FILTER_OBJ, 5000);
    return 0;
}

static int afxdp_socket_create(struct afxdp_ctx *ctx,
                               const char *ifname,
                               uint32_t queue_id,
                               uint32_t xdp_flags,
                               uint16_t bind_flags)
{
    struct xsk_socket_config xsk_cfg;
    int ret;

    memset(&xsk_cfg, 0, sizeof(xsk_cfg));
    xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    xsk_cfg.xdp_flags = xdp_flags;
    xsk_cfg.bind_flags = bind_flags;

    /* If our custom filter is loaded, don't let libxdp load the default
     * catch-all program that would steal TCP packets on loopback. */
    if (ctx->xdp_prog)
        xsk_cfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;

    ret = xsk_socket__create(&ctx->xsk, ifname, queue_id, ctx->umem, &ctx->rx, &ctx->tx, &xsk_cfg);
    if (ret && ctx->xsk) {
        xsk_socket__delete(ctx->xsk);
        ctx->xsk = NULL;
    }

    /* Register socket in our custom xsks_map so the BPF program can redirect. */
    if (ret == 0 && ctx->xdp_prog) {
        int sock_fd = xsk_socket__fd(ctx->xsk);
        __u32 key = queue_id;
        if (bpf_map_update_elem(ctx->xsks_map_fd, &key, &sock_fd, 0) != 0) {
            fprintf(stderr, "[afxdp] failed to update xsks_map: %s\n", strerror(errno));
            xsk_socket__delete(ctx->xsk);
            ctx->xsk = NULL;
            return -1;
        }
    }

    return ret;
}

static int afxdp_env_flag_enabled(const char *name)
{
    const char *value = getenv(name);
    return value && value[0] != '\0' && strcmp(value, "0") != 0;
}

static void afxdp_log_create_error(const char *ifname,
                                   uint32_t queue_id,
                                   const char *mode,
                                   int ret)
{
    int err = ret < 0 ? -ret : ret;
    fprintf(stderr,
            "[afxdp] %s create failed on %s queue=%u: %s\n",
            mode,
            ifname,
            queue_id,
            strerror(err));
}

int afxdp_init(struct afxdp_ctx **out,
               const char *ifname,
               uint32_t queue_id,
               int force_zerocopy)
{
    struct afxdp_ctx *ctx = NULL;
    struct xsk_umem_config umem_cfg;
    struct rlimit rlim = {RLIM_INFINITY, RLIM_INFINITY};
    uint16_t bind_flags = XDP_USE_NEED_WAKEUP;
    int ifindex;
    int prefer_skb;
    size_t packet_buffer_size;
    int ret;

    if (!out || !ifname || ifname[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    *out = NULL;

    ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        errno = ENODEV;
        return -1;
    }

    (void)setrlimit(RLIMIT_MEMLOCK, &rlim);

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;

    packet_buffer_size = (size_t)AFXDP_NUM_FRAMES * AFXDP_FRAME_SIZE;
    ret = posix_memalign(&ctx->buffer, getpagesize(), packet_buffer_size);
    if (ret != 0) {
        errno = ret;
        goto error;
    }

    memset(&umem_cfg, 0, sizeof(umem_cfg));
    umem_cfg.fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    umem_cfg.comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    umem_cfg.frame_size = AFXDP_FRAME_SIZE;

    ret = xsk_umem__create(&ctx->umem,
                           ctx->buffer,
                           packet_buffer_size,
                           &ctx->fq,
                           &ctx->cq,
                           &umem_cfg);
    if (ret) {
        errno = -ret;
        goto error;
    }

    for (uint32_t i = 0; i < AFXDP_NUM_FRAMES; i++)
        ctx->umem_frame_addr[i] = (uint64_t)i * AFXDP_FRAME_SIZE;
    ctx->umem_frame_free = AFXDP_NUM_FRAMES;

    if (force_zerocopy)
        bind_flags |= XDP_ZEROCOPY;

    prefer_skb = afxdp_env_flag_enabled("HFT_AFXDP_PREFER_SKB");

    /* Load custom XDP filter that only redirects UDP feed packets to AF_XDP,
     * letting TCP (order sender) pass through the normal kernel stack.
     * Falls back to the default catch-all if the .o file is missing. */
    {
        uint32_t xdp_fl = prefer_skb ? XDP_FLAGS_SKB_MODE : 0;
        if (afxdp_load_xdp_filter(ctx, ifindex, xdp_fl) != 0)
            fprintf(stderr, "[afxdp] warning: custom filter not loaded, "
                    "TCP on same interface may be intercepted\n");
    }

    if (prefer_skb) {
        bind_flags &= (uint16_t)~XDP_ZEROCOPY;
        ret = afxdp_socket_create(ctx, ifname, queue_id, XDP_FLAGS_SKB_MODE, bind_flags);
        if (ret == 0)
            goto created;

        afxdp_log_create_error(ifname, queue_id, "SKB mode", ret);
    } else {
        ret = afxdp_socket_create(ctx, ifname, queue_id, 0, bind_flags);
    }

    if (ret && force_zerocopy && !prefer_skb) {
        bind_flags &= (uint16_t)~XDP_ZEROCOPY;
        ret = afxdp_socket_create(ctx, ifname, queue_id, 0, bind_flags);
        if (ret == 0) {
            fprintf(stderr,
                    "[afxdp] zero-copy unavailable on %s queue=%u, using copy mode\n",
                    ifname,
                    queue_id);
        }
    }

    if (ret) {
        bind_flags &= (uint16_t)~XDP_ZEROCOPY;
        ret = afxdp_socket_create(ctx, ifname, queue_id, XDP_FLAGS_SKB_MODE, bind_flags);
        if (ret == 0) {
            fprintf(stderr,
                    "[afxdp] native XDP unavailable on %s queue=%u, falling back to SKB mode\n",
                    ifname,
                    queue_id);
            goto created;
        }

        afxdp_log_create_error(ifname, queue_id, "SKB fallback", ret);

        if (ret == -EBUSY) {
            (void)bpf_xdp_detach(ifindex, XDP_FLAGS_SKB_MODE, NULL);
            (void)bpf_xdp_detach(ifindex, 0, NULL);

            ret = afxdp_socket_create(ctx, ifname, queue_id, XDP_FLAGS_SKB_MODE, bind_flags);
            if (ret == 0) {
                fprintf(stderr,
                        "[afxdp] recovered from busy XDP state on %s queue=%u via detach+retry\n",
                        ifname,
                        queue_id);
                goto created;
            }

            afxdp_log_create_error(ifname, queue_id, "SKB retry", ret);
        }
    }

    if (ret) {
        errno = -ret;
        goto error;
    }

created:
    if (afxdp_refill_fq(ctx) < 0)
        goto error;

    *out = ctx;
    return 0;

error:
    afxdp_close(ctx);
    return -1;
}

int afxdp_recv_burst(struct afxdp_ctx *ctx,
                     struct afxdp_pkt *pkts,
                     int max_pkts)
{
    uint32_t idx_rx = 0;
    uint32_t rcvd;

    if (!ctx || !pkts || max_pkts <= 0) {
        errno = EINVAL;
        return -1;
    }

    if (afxdp_refill_fq(ctx) < 0)
        return -1;

    rcvd = xsk_ring_cons__peek(&ctx->rx, (uint32_t)max_pkts, &idx_rx);
    if (!rcvd)
        return 0;

    for (uint32_t i = 0; i < rcvd; i++) {
        const struct xdp_desc *desc = xsk_ring_cons__rx_desc(&ctx->rx, idx_rx++);
        pkts[i].data = xsk_umem__get_data(ctx->buffer, desc->addr);
        pkts[i].len = desc->len;
        pkts[i].addr = desc->addr;
    }

    xsk_ring_cons__release(&ctx->rx, rcvd);
    return (int)rcvd;
}

int afxdp_recycle(struct afxdp_ctx *ctx,
                  const uint64_t *addrs,
                  int n)
{
    if (!ctx || n < 0 || (n > 0 && !addrs)) {
        errno = EINVAL;
        return -1;
    }

    for (int i = 0; i < n; i++) {
        if (xsk_free_umem_frame(ctx, addrs[i]) != 0)
            return -1;
    }

    if (afxdp_refill_fq(ctx) < 0)
        return -1;

    return 0;
}

void afxdp_close(struct afxdp_ctx *ctx)
{
    if (!ctx)
        return;

    if (ctx->xsk)
        xsk_socket__delete(ctx->xsk);
    if (ctx->xdp_prog) {
        enum xdp_attach_mode mode = XDP_MODE_SKB;
        xdp_program__detach(ctx->xdp_prog, ctx->ifindex, mode, 0);
        xdp_program__close(ctx->xdp_prog);
    }
    if (ctx->umem)
        xsk_umem__delete(ctx->umem);

    free(ctx->buffer);
    free(ctx);
}
