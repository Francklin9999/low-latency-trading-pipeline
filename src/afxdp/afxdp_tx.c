#define _GNU_SOURCE
#define _DEFAULT_SOURCE

// AF_XDP TX implementation. See include/hft/afxdp/afxdp_tx.h for design.

#include "hft/afxdp/afxdp_tx.h"
#include "hft/utils/checksum.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <xdp/xsk.h>

#define AFXDP_TX_NUM_FRAMES 512u
#define AFXDP_TX_FRAME_SIZE 4096u
#define AFXDP_TX_UMEM_SIZE  (AFXDP_TX_NUM_FRAMES * AFXDP_TX_FRAME_SIZE)
#define HUGE_PAGE_SIZE_2MB  (2u * 1024u * 1024u)

#define ETH_HDR_LEN  14
#define IP_HDR_LEN   20
#define UDP_HDR_LEN  8
#define HDR_TOTAL    (ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN)
#define MAX_PAYLOAD  (AFXDP_TX_FRAME_SIZE - HDR_TOTAL)

struct afxdp_tx_ctx {
    struct xsk_ring_prod fq;     // unused for TX-only, but UMEM requires
    struct xsk_ring_cons cq;
    struct xsk_ring_prod tx;
    struct xsk_ring_cons rx;     // unused

    struct xsk_umem   *umem;
    struct xsk_socket *xsk;

    void   *umem_buffer;
    size_t  umem_buffer_size;
    int     used_hugepages;

    // Frame 0 holds the immutable template; frames 1..N-1 are the TX pool.
    uint64_t  free_frames[AFXDP_TX_NUM_FRAMES];
    uint32_t  free_count;

    // Template lives in frame 0; ip_csum_baseline is the template IP checksum
    // for tot_len == MAX_PAYLOAD, used by the hot path's incremental update.
    uint8_t  *template_frame;
    uint32_t  payload_offset;
    uint16_t  ip_csum_baseline;

    struct afxdp_tx_config cfg;
};

// Compute IPv4 header checksum: zero the `check` field beforehand, then
// cksum_partial_simd over the header and complement the fold.
static uint16_t ip_checksum(const void *buf, uint32_t len)
{
    return (uint16_t)~cksum_fold16(cksum_partial_simd(buf, len, 0));
}

// RFC 1624 incremental update: replace 16-bit half-word `old_word` with
// `new_word`. Operates on the complement of the checksum for simpler arithmetic.
static uint16_t csum_replace2(uint16_t old_csum, uint16_t old_word, uint16_t new_word)
{
    uint32_t sum = (uint16_t)~old_csum;
    sum += (uint16_t)~old_word;
    sum += new_word;
    return (uint16_t)~cksum_fold16(sum);
}

static void *alloc_umem_region(size_t size, int try_hugepages, int *used_huge)
{
    *used_huge = 0;
#ifdef MAP_HUGETLB
    if (try_hugepages) {
        const int huge_flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB
#ifdef MAP_HUGE_2MB
            | MAP_HUGE_2MB
#endif
            ;
        void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, huge_flags, -1, 0);
        if (p != MAP_FAILED) {
            *used_huge = 1;
            return p;
        }
        // Most likely cause: /proc/sys/vm/nr_hugepages is 0. Fall back.
    }
#endif
    void *p = NULL;
    if (posix_memalign(&p, getpagesize(), size) != 0) return NULL;
    return p;
}

static void free_umem_region(void *p, size_t size, int used_huge)
{
    if (!p) return;
    if (used_huge) (void)munmap(p, size);
    else free(p);
}

// Lay out one Ethernet/IP/UDP template in the first UMEM frame. Stable fields
// are written once; the hot path patches payload + checksums per send (RFC 1624).
static void build_template(struct afxdp_tx_ctx *ctx, uint32_t payload_len)
{
    uint8_t *frame = ctx->template_frame;
    memset(frame, 0, HDR_TOTAL);

    // Ethernet.
    memcpy(frame, ctx->cfg.dst_mac, 6);
    memcpy(frame + 6, ctx->cfg.src_mac, 6);
    frame[12] = (uint8_t)(ETH_P_IP >> 8);
    frame[13] = (uint8_t)(ETH_P_IP & 0xFF);

    // IPv4.
    struct iphdr *ip = (struct iphdr *)(frame + ETH_HDR_LEN);
    ip->version  = 4;
    ip->ihl      = IP_HDR_LEN / 4;
    ip->tos      = 0;
    ip->tot_len  = htons(IP_HDR_LEN + UDP_HDR_LEN + payload_len);
    ip->id       = 0;
    ip->frag_off = 0;
    ip->ttl      = 64;
    ip->protocol = IPPROTO_UDP;
    ip->check    = 0;
    ip->saddr    = ctx->cfg.src_ip_be;
    ip->daddr    = ctx->cfg.dst_ip_be;
    ip->check    = ip_checksum(ip, IP_HDR_LEN);
    ctx->ip_csum_baseline = ip->check;

    // UDP.
    struct udphdr *udp = (struct udphdr *)(frame + ETH_HDR_LEN + IP_HDR_LEN);
    udp->source = htons(ctx->cfg.src_port);
    udp->dest   = htons(ctx->cfg.dst_port);
    udp->len    = htons(UDP_HDR_LEN + payload_len);

    // UDP checksum is recomputed per send against the actual payload; the
    // template field stays zero and the hot path overwrites it.
    udp->check = 0;
}

static int xsk_create_tx(struct afxdp_tx_ctx *ctx)
{
    struct xsk_socket_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.rx_size      = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    cfg.tx_size      = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    cfg.bind_flags   = XDP_USE_NEED_WAKEUP;
    cfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD; // RX side owns it

    if (ctx->cfg.force_zerocopy) cfg.bind_flags |= XDP_ZEROCOPY;

    int ret = xsk_socket__create(&ctx->xsk, ctx->cfg.ifname, ctx->cfg.queue_id,
                                 ctx->umem, &ctx->rx, &ctx->tx, &cfg);
    if (ret == 0) return 0;

    // If zero-copy refused, fall back.
    if (ret == -EOPNOTSUPP || ret == -ENOTSUP) {
        cfg.bind_flags &= (uint16_t)~XDP_ZEROCOPY;
        ret = xsk_socket__create(&ctx->xsk, ctx->cfg.ifname, ctx->cfg.queue_id,
                                 ctx->umem, &ctx->rx, &ctx->tx, &cfg);
    }
    return ret;
}

int afxdp_tx_init(struct afxdp_tx_ctx **out, const struct afxdp_tx_config *cfg)
{
    if (!out || !cfg || !cfg->ifname) { errno = EINVAL; return -1; }

    struct rlimit rlim = {RLIM_INFINITY, RLIM_INFINITY};
    (void)setrlimit(RLIMIT_MEMLOCK, &rlim);

    struct afxdp_tx_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return -1;
    ctx->cfg              = *cfg;
    ctx->umem_buffer_size = AFXDP_TX_UMEM_SIZE;

    // The pool fits one 2MB hugepage by construction (512 * 4096 = 2MB), so
    // a single rounded-up region is exact.
    ctx->umem_buffer = alloc_umem_region(ctx->umem_buffer_size,
                                         cfg->use_hugepages,
                                         &ctx->used_hugepages);
    if (!ctx->umem_buffer) goto err;

    struct xsk_umem_config uc;
    memset(&uc, 0, sizeof(uc));
    uc.fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    uc.comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    uc.frame_size = AFXDP_TX_FRAME_SIZE;
    if (xsk_umem__create(&ctx->umem, ctx->umem_buffer, ctx->umem_buffer_size,
                         &ctx->fq, &ctx->cq, &uc) != 0) {
        goto err;
    }

    if (xsk_create_tx(ctx) != 0) goto err;

    // Frame 0: template; frames 1..N-1: TX pool.
    ctx->template_frame = (uint8_t *)ctx->umem_buffer;
    ctx->payload_offset = HDR_TOTAL;
    for (uint32_t i = AFXDP_TX_NUM_FRAMES - 1; i >= 1; --i) {
        ctx->free_frames[ctx->free_count++] =
            (uint64_t)i * AFXDP_TX_FRAME_SIZE;
    }

    // Build a template sized for our largest expected payload; per-send we
    // rebuild only the length-dependent fields (totals + UDP len).
    build_template(ctx, MAX_PAYLOAD);

    fprintf(stderr,
            "[afxdp_tx] init iface=%s queue=%u frames=%u hugepages=%d\n",
            ctx->cfg.ifname, ctx->cfg.queue_id, AFXDP_TX_NUM_FRAMES,
            ctx->used_hugepages);

    *out = ctx;
    return 0;

err:
    afxdp_tx_close(ctx);
    return -1;
}

static uint64_t alloc_frame(struct afxdp_tx_ctx *ctx)
{
    if (ctx->free_count == 0) return UINT64_MAX;
    return ctx->free_frames[--ctx->free_count];
}

static void release_frame(struct afxdp_tx_ctx *ctx, uint64_t addr)
{
    if (ctx->free_count >= AFXDP_TX_NUM_FRAMES) return;
    ctx->free_frames[ctx->free_count++] = addr;
}

int afxdp_tx_drain_completions(struct afxdp_tx_ctx *ctx)
{
    if (!ctx) return 0;
    uint32_t idx = 0;
    uint32_t n = xsk_ring_cons__peek(&ctx->cq, AFXDP_TX_NUM_FRAMES, &idx);
    if (!n) return 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t addr = *xsk_ring_cons__comp_addr(&ctx->cq, idx++);
        release_frame(ctx, addr);
    }
    xsk_ring_cons__release(&ctx->cq, n);
    return (int)n;
}

int afxdp_tx_send(struct afxdp_tx_ctx *ctx,
                  const void *payload, uint32_t payload_len)
{
    if (!ctx || !payload || payload_len == 0 || payload_len > MAX_PAYLOAD) {
        errno = EINVAL;
        return -1;
    }

    // Best-effort completion drain: keeps the free list topped up. Cheap
    // peek-and-release; <100 ns when nothing to drain.
    afxdp_tx_drain_completions(ctx);

    uint64_t addr = alloc_frame(ctx);
    if (addr == UINT64_MAX) {
        // TX backpressure: caller retries.
        errno = ENOSPC;
        return -1;
    }

    const uint32_t pkt_len = HDR_TOTAL + payload_len;
    uint8_t *frame = (uint8_t *)xsk_umem__get_data(ctx->umem_buffer, addr);

    // Copy template -- the headers are the same for every order.
    memcpy(frame, ctx->template_frame, HDR_TOTAL);

    // Patch IP/UDP length fields, using incremental checksum updates so we
    // don't re-scan the full header each send.
    struct iphdr *ip = (struct iphdr *)(frame + ETH_HDR_LEN);
    struct udphdr *udp = (struct udphdr *)(frame + ETH_HDR_LEN + IP_HDR_LEN);

    const uint16_t old_ip_tot   = ip->tot_len;
    const uint16_t old_udp_len  = udp->len;
    const uint16_t new_ip_tot   = htons((uint16_t)(IP_HDR_LEN + UDP_HDR_LEN + payload_len));
    const uint16_t new_udp_len  = htons((uint16_t)(UDP_HDR_LEN + payload_len));

    ip->tot_len = new_ip_tot;
    udp->len    = new_udp_len;
    ip->check   = csum_replace2(ip->check, old_ip_tot, new_ip_tot);

    // Drop payload into place.
    memcpy(frame + HDR_TOTAL, payload, payload_len);

    // UDP checksum: recompute from pseudo-header + UDP header + payload in one
    // pass. Payload is small; the checksum field must be zero during compute.
    udp->check = 0;
    uint32_t pseudo = 0;
    pseudo += (ctx->cfg.src_ip_be & 0xFFFF) + ((ctx->cfg.src_ip_be >> 16) & 0xFFFF);
    pseudo += (ctx->cfg.dst_ip_be & 0xFFFF) + ((ctx->cfg.dst_ip_be >> 16) & 0xFFFF);
    pseudo += htons(IPPROTO_UDP);
    pseudo += new_udp_len;
    pseudo = cksum_partial_simd(udp, UDP_HDR_LEN + payload_len, pseudo);
    udp->check = (uint16_t)~cksum_fold16(pseudo);
    if (udp->check == 0) udp->check = 0xFFFF;

    // Push descriptor onto the TX ring.
    uint32_t tx_idx = 0;
    if (xsk_ring_prod__reserve(&ctx->tx, 1, &tx_idx) != 1) {
        release_frame(ctx, addr);
        errno = EAGAIN;
        return -1;
    }
    struct xdp_desc *desc = xsk_ring_prod__tx_desc(&ctx->tx, tx_idx);
    desc->addr = addr;
    desc->len  = pkt_len;
    xsk_ring_prod__submit(&ctx->tx, 1);

    // Kick the kernel only if the ring asks us to; with XDP_USE_NEED_WAKEUP a
    // busy NIC TX queue picks up the descriptor on its own, saving the syscall.
    if (xsk_ring_prod__needs_wakeup(&ctx->tx)) {
        (void)sendto(xsk_socket__fd(ctx->xsk), NULL, 0, MSG_DONTWAIT,
                     NULL, 0);
    }

    return 0;
}

void afxdp_tx_close(struct afxdp_tx_ctx *ctx)
{
    if (!ctx) return;
    if (ctx->xsk) xsk_socket__delete(ctx->xsk);
    if (ctx->umem) xsk_umem__delete(ctx->umem);
    free_umem_region(ctx->umem_buffer, ctx->umem_buffer_size, ctx->used_hugepages);
    free(ctx);
}
