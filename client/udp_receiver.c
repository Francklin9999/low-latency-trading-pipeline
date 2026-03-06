#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <endian.h>
#include <arpa/inet.h>
#include <stdatomic.h>
#include <limits.h>
#include <unistd.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include "../utils/moldudp64.h"
#include "../utils/utils.h"
#include "../afxdp/afxdp_receiver.h"
#include "client.h"
#include "config.h"
#include "../ring_buffers/parser/parser_to_engine.h"
#include "../ring_buffers/event/event_to_engine.h"
#include "../parser/parser_to_engine.h"


#define RX_BURST_SIZE 128

event_to_engine* EVENT_ENGINE = NULL;
order_to_exc* ORDER_TO_EXC = NULL;

parser_to_engine PARSER_ENGINE = {
    .next_write = 0,
    .next_read = 0,
};

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Extract MoldUDP64 payload from a raw AF_XDP frame.
 * Two-level probe: Ethernet-framed first, raw-IP fallback if no eth header. */
static int extract_feed_payload(const uint8_t *frame, uint32_t frame_len,
                                const uint8_t **payload, uint32_t *payload_len)
{
    const struct udphdr *udp;
    size_t offset = 0;

    if (!frame || !payload || !payload_len)
        return -1;

    *payload = NULL;
    *payload_len = 0;

    /* --- Ethernet-framed path --- */
    if (frame_len >= sizeof(struct ethhdr)) {
        uint16_t proto;

        offset = sizeof(struct ethhdr);
        proto = ntohs(((const struct ethhdr *)frame)->h_proto);

        /* Strip 802.1Q / 802.1ad VLAN tags. */
        while (proto == ETH_P_8021Q || proto == ETH_P_8021AD) {
            if (frame_len < offset + 4)
                return -1;
            proto = ((uint16_t)frame[offset + 2] << 8) | frame[offset + 3];
            offset += 4;
        }

        if (proto == ETH_P_IP) {
            const struct iphdr* ip4;
            uint32_t ihl;

            if (frame_len < offset + sizeof(struct iphdr))
                return -1;

            ip4 = (const struct iphdr *)(frame + offset);
            ihl = (uint32_t)ip4->ihl * 4u;
            if (ihl < sizeof(struct iphdr) || frame_len < offset + ihl + sizeof(struct udphdr))
                return -1;
            if (ip4->protocol != IPPROTO_UDP)
                return -1;

            offset += ihl;
            udp = (const struct udphdr *)(frame + offset);
            if (ntohs(udp->dest) != FEED_PORT)
                return -1;

            offset += sizeof(struct udphdr);
            *payload = frame + offset;
            *payload_len = frame_len - (uint32_t)offset;
            return 0;
        }

        if (proto == ETH_P_IPV6) {
            const struct ipv6hdr *ip6;

            if (frame_len < offset + sizeof(struct ipv6hdr) + sizeof(struct udphdr))
                return -1;

            ip6 = (const struct ipv6hdr *)(frame + offset);
            if (ip6->nexthdr != IPPROTO_UDP)
                return -1;

            offset += sizeof(struct ipv6hdr);
            udp = (const struct udphdr *)(frame + offset);
            if (ntohs(udp->dest) != FEED_PORT)
                return -1;

            offset += sizeof(struct udphdr);
            *payload = frame + offset;
            *payload_len = frame_len - (uint32_t)offset;
            return 0;
        }

        /* Unknown EtherType — don't fall through; frame[0] is a MAC byte. */
        return -1;
    }

    /* --- Raw-IP path (no Ethernet header) --- */

    if (frame_len >= sizeof(struct iphdr) && ((frame[0] >> 4) == 4)) {
        const struct iphdr* ip4 = (const struct iphdr*) frame;
        uint32_t ihl = (uint32_t)ip4->ihl * 4u;

        if (ihl < sizeof(struct iphdr) || frame_len < ihl + sizeof(struct udphdr))
            return -1;
        if (ip4->protocol != IPPROTO_UDP)
            return -1;

        udp = (const struct udphdr *)(frame + ihl);
        if (ntohs(udp->dest) != FEED_PORT)
            return -1;

        *payload = frame + ihl + sizeof(struct udphdr);
        *payload_len = frame_len - (uint32_t)(ihl + sizeof(struct udphdr));
        return 0;
    }

    if (frame_len >= sizeof(struct ipv6hdr) && ((frame[0] >> 4) == 6)) {
        const struct ipv6hdr* ip6 = (const struct ipv6hdr *)frame;

        if (ip6->nexthdr != IPPROTO_UDP || frame_len < sizeof(struct ipv6hdr) + sizeof(struct udphdr))
            return -1;

        udp = (const struct udphdr*) (frame + sizeof(struct ipv6hdr));
        if (ntohs(udp->dest) != FEED_PORT)
            return -1;

        *payload = frame + sizeof(struct ipv6hdr) + sizeof(struct udphdr);
        *payload_len = frame_len - (uint32_t)(sizeof(struct ipv6hdr) + sizeof(struct udphdr));
        return 0;
    }

    return -1;
}

static uint32_t env_u32_or_default(const char* name, uint32_t fallback)
{
    char *end = NULL;
    unsigned long parsed;
    const char *value = getenv(name);

    if (!value || value[0] == '\0')
        return fallback;

    parsed = strtoul(value, &end, 10);
    if (*end != '\0' || parsed > UINT_MAX)
        return fallback;

    return (uint32_t)parsed;
}

/* Push consumed UMEM frame addresses back to the AF_XDP fill ring.
 * recycle_read tracks how far we have already recycled (separate from
 * PARSER_ENGINE.next_read so we can batch the afxdp_recycle calls). */
static int recycle_consumed_frames(struct afxdp_ctx* xsk, uint32_t* recycle_read)
{
    uint64_t addrs[RX_BURST_SIZE];
    /* Snapshot the consumer cursor once; it only moves forward. */
    uint32_t consumed = atomic_load_explicit(&PARSER_ENGINE.next_read, memory_order_acquire);
    int n = 0;

    while (*recycle_read != consumed) {
        packet_ref* ref = PARSER_ENGINE.data[*recycle_read & PARSER_ENGINE_MASK];
        addrs[n++] = ref->umem_addr; /* address token returned to the fill ring */
        (*recycle_read)++;

        if (n == RX_BURST_SIZE) {   /* flush when the batch is full */
            if (afxdp_recycle(xsk, addrs, n) != 0)
                return -1;
            n = 0;
        }
    }

    if (n > 0 && afxdp_recycle(xsk, addrs, n) != 0) /* flush remainder */
        return -1;

    return 0;
}

void *parser_to_engine_thread(void* arg) {
    (void) arg;
    for (;;)
        parse_to_engine();
    return NULL;
}

void *udp_receiver_thread(void *arg) {
    (void)arg;

    int fd_event = shm_open(SHM_EVENT_TO_STRAT, O_RDWR, 0);
    if (fd_event == -1) {
        fprintf(stderr, "shm_open(%s) failed: %s\n",
                SHM_EVENT_TO_STRAT, strerror(errno));
        _exit(1);
    }

    EVENT_ENGINE = (event_to_engine*) mmap(
        NULL, sizeof(event_to_engine), PROT_READ | PROT_WRITE,
        MAP_SHARED, fd_event, 0);
    if (EVENT_ENGINE == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %s\n", strerror(errno));
        close(fd_event);
        _exit(1);
    }

    close(fd_event);

    struct afxdp_ctx* xsk = NULL;
    struct afxdp_pkt pkts[RX_BURST_SIZE];
    const char* ifname = getenv("HFT_AFXDP_IFACE");
    const char* active_ifname = NULL;
    uint32_t queue_id = env_u32_or_default("HFT_AFXDP_QUEUE", 0);
    int force_zerocopy = env_u32_or_default("HFT_AFXDP_FORCE_ZEROCOPY", 0) != 0;
    uint32_t recycle_read = 0;

    if (!ifname || ifname[0] == '\0') {
        fprintf(stderr, "HFT_AFXDP_IFACE not set, defaulting to lo\n");
        ifname = "lo";
    }
    active_ifname = ifname;

    atomic_store_explicit(&PARSER_ENGINE.next_write, 0, memory_order_relaxed);
    atomic_store_explicit(&PARSER_ENGINE.next_read, 0, memory_order_relaxed);

    int zc = force_zerocopy;

    if (afxdp_init(&xsk, active_ifname, queue_id, zc) != 0) {
        if (zc) {
            fprintf(stderr, "[udp_receiver] AF_XDP zero-copy failed; retrying copy mode\n");
            zc = 0;
            if (afxdp_init(&xsk, active_ifname, queue_id, zc) != 0) {
                if (strcmp(active_ifname, "lo") != 0) {
                    fprintf(stderr,
                            "[udp_receiver] AF_XDP init failed on iface=%s, retrying loopback iface=lo\n",
                            active_ifname);
                    active_ifname = "lo";
                    if (afxdp_init(&xsk, active_ifname, queue_id, 0) != 0) {
                        perror("[udp_receiver] afxdp_init (copy mode + lo)");
                        return NULL;
                    }
                } else {
                    perror("[udp_receiver] afxdp_init (copy mode)");
                    return NULL;
                }
            }
        } else {
            if (strcmp(active_ifname, "lo") != 0) {
                fprintf(stderr,
                        "[udp_receiver] AF_XDP init failed on iface=%s, retrying loopback iface=lo\n",
                        active_ifname);
                active_ifname = "lo";
                if (afxdp_init(&xsk, active_ifname, queue_id, 0) != 0) {
                    perror("[udp_receiver] afxdp_init (lo)");
                    return NULL;
                }
                zc = 0;
            } else {
                perror("[udp_receiver] afxdp_init");
                return NULL;
            }
        }
    }

    printf("[udp_receiver] AF_XDP listening on iface=%s queue=%u zerocopy=%d\n",
           active_ifname,
           queue_id,
           zc);

    pthread_t event_engine;
    pthread_create(&event_engine, NULL, parser_to_engine_thread, NULL);

    while (1) {
        /* Return UMEM frames the engine has finished reading. */
        if (recycle_consumed_frames(xsk, &recycle_read) != 0) {
            perror("[udp_receiver] recycle_consumed");
            break;
        }

        /* Pull up to RX_BURST_SIZE raw frames from the NIC. */
        int n = afxdp_recv_burst(xsk, pkts, RX_BURST_SIZE);
        if (n < 0) {
            perror("[udp_receiver] afxdp_recv_burst");
            break;
        }

        if (n == 0) {
            cpu_relax();
            continue;
        }

        /* Snapshot both cursors once for the whole burst. */
        uint32_t w = atomic_load_explicit(&PARSER_ENGINE.next_write, memory_order_relaxed);
        uint32_t r = atomic_load_explicit(&PARSER_ENGINE.next_read, memory_order_acquire);
        int stop = 0;
        uint64_t drop_addrs[RX_BURST_SIZE]; /* UMEM addresses to recycle immediately */
        int drop_count = 0;

        for (int i = 0; i < n; i++) {
            const uint8_t* payload;
            uint32_t payload_len;

            /* Not a UDP datagram on FEED_PORT — queue for immediate recycle. */
            if (extract_feed_payload(pkts[i].data, pkts[i].len, &payload, &payload_len) != 0) {
                drop_addrs[drop_count++] = pkts[i].addr;
                continue;
            }
            /* Too short to hold a MoldUDP64 header — malformed, recycle. */
            if ((size_t)payload_len < sizeof(mold_udp64_header)) {
                drop_addrs[drop_count++] = pkts[i].addr;
                continue;
            }

            const mold_udp64_header* hdr = (const mold_udp64_header*) payload;

            /* message_count 0xFFFF is the MoldUDP64 end-of-session signal. */
            if (be16toh(hdr->message_count) == 0xFFFF) {
                printf("[udp_receiver] end of session at seq %lu\n",
                       (unsigned long)be64toh(hdr->sequence_number));
                drop_addrs[drop_count++] = pkts[i].addr;
                stop = 1;
                break;
            }

            /* Engine is too slow — drop; sequence gap triggers retransmit. */
            if (w - r >= PARSER_TO_ENGINE_SIZE) {
                fprintf(stderr, "[udp_receiver] ring buffer full, dropping packet\n");
                drop_addrs[drop_count++] = pkts[i].addr;
                continue;
            }

            /* Zero-copy publish: overlay packet_ref at the start of the UMEM
             * frame (reusing header bytes already consumed by extract_feed_payload).
             * Parser expects a sequence of length-prefixed ITCH messages, so skip
             * the 20-byte MoldUDP64 header and publish only message block bytes.
             * sizeof(packet_ref)==28 bytes < 42-byte minimum header overhead. */
            uint32_t slot = w & PARSER_ENGINE_MASK;
            packet_ref* ref = (packet_ref*)pkts[i].data;
            ref->data = payload + sizeof(mold_udp64_header);
            ref->umem_addr = pkts[i].addr; /* returned to fill ring after consume */
            ref->len = payload_len - (uint32_t)sizeof(mold_udp64_header);
            ref->ts = now_ns();
            PARSER_ENGINE.data[slot] = ref;

            w++;
        }

        /* Publish the updated write cursor to the engine. */
        atomic_store_explicit(&PARSER_ENGINE.next_write, w, memory_order_release);

        /* Recycle dropped / end-of-session frames right away. */
        if (drop_count > 0 && afxdp_recycle(xsk, drop_addrs, drop_count) != 0) {
            perror("[udp_receiver] recycle_drop");
            break;
        }

        if (stop)
            break;
    }

    /* Drain remaining consumed frames before tearing down AF_XDP. */
    if (recycle_consumed_frames(xsk, &recycle_read) != 0)
        perror("[udp_receiver] recycle_consumed");

    afxdp_close(xsk);
    return NULL;
}
