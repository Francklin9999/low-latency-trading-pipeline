#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
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
#include "hft/utils/moldudp64.h"
#include "hft/utils/utils.h"
#include "hft/afxdp/afxdp_receiver.h"
#include "hft/client/client.h"
#include "hft/server/server.h"
#include "hft/ring_buffers/parser/parser_to_engine.h"


#define RX_BURST_SIZE 128

// Reorder pool capacity. Each slot is ~1.5 KB; sized to absorb several ms
// of feed at burst rate while the rewinder is filling a gap.
#define REORDER_MAX            512
#define RETRANSMIT_INTERVAL_NS 5000000ULL  // 5 ms between repeat asks
// High bit of packet_ref::umem_addr distinguishes reorder-pool slots from
// UMEM frames in the recycle path.
#define REORDER_ADDR_FLAG      (1ULL << 63)
#define REORDER_ADDR_IDX_MASK  (REORDER_MAX - 1)

typedef struct {
    packet_ref ref;             // what we publish to PARSER_ENGINE
    uint64_t   start_seq;       // MoldUDP64 sequence number
    uint16_t   msg_count;       // # of ITCH messages in this packet
    uint16_t   payload_len;     // bytes valid in `bytes`
    uint8_t    used;            // slot occupancy
    uint8_t    bytes[1500];     // copy of full UDP payload
} reorder_slot;

static reorder_slot reorder_pool[REORDER_MAX];

// Sequencing state for the active session. session==0xFF... means unset.
static unsigned char gap_session[10];
static int           gap_session_set = 0;
static uint64_t      gap_expected_seq = 0;
static uint64_t      gap_last_request_ns = 0;
static int           gap_rewinder_fd = -1;
static struct sockaddr_in gap_rewinder_addr;
static uint64_t      gap_dropped_packets = 0;
static uint64_t      gap_retrans_requests = 0;
static uint64_t      gap_retrans_filled = 0;

// SPSC: udp_receiver produces, dispatcher consumes.
parser_to_engine PARSER_ENGINE = {
    .next_write = 0,
    .next_read = 0,
};

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// Extract MoldUDP64 payload from a raw AF_XDP frame.
// Two-level probe: Ethernet-framed first, raw-IP fallback if no eth header.
static int extract_feed_payload(const uint8_t *frame, uint32_t frame_len,
                                const uint8_t **payload, uint32_t *payload_len)
{
    const struct udphdr *udp;
    size_t offset = 0;

    if (!frame || !payload || !payload_len)
        return -1;

    *payload = NULL;
    *payload_len = 0;

    // Ethernet-framed path
    if (frame_len >= sizeof(struct ethhdr)) {
        uint16_t proto;

        offset = sizeof(struct ethhdr);
        proto = ntohs(((const struct ethhdr *)frame)->h_proto);

        // Strip 802.1Q / 802.1ad VLAN tags.
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

        // Unknown EtherType -- don't fall through; frame[0] is a MAC byte.
        return -1;
    }

    // Raw-IP path (no Ethernet header)

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

// Slot states: used=0 free; used=1 & start_seq>=expected pending; used=1 &
// start_seq<expected in-flight. find/find_min/count skip in-flight slots.
static int reorder_alloc_slot(void)
{
    for (int i = 0; i < REORDER_MAX; i++) {
        if (!reorder_pool[i].used) return i;
    }
    return -1;
}

static reorder_slot *reorder_find(uint64_t seq)
{
    for (int i = 0; i < REORDER_MAX; i++) {
        if (!reorder_pool[i].used) continue;
        if (reorder_pool[i].start_seq < gap_expected_seq) continue;  // in-flight
        if (reorder_pool[i].start_seq == seq)
            return &reorder_pool[i];
    }
    return NULL;
}

// Find the smallest-seq pending entry (excluding in-flight).
static reorder_slot *reorder_find_min(void)
{
    reorder_slot *best = NULL;
    for (int i = 0; i < REORDER_MAX; i++) {
        if (!reorder_pool[i].used) continue;
        if (reorder_pool[i].start_seq < gap_expected_seq) continue;
        if (!best || reorder_pool[i].start_seq < best->start_seq)
            best = &reorder_pool[i];
    }
    return best;
}

static int reorder_count(void)
{
    int n = 0;
    for (int i = 0; i < REORDER_MAX; i++) {
        if (!reorder_pool[i].used) continue;
        if (reorder_pool[i].start_seq < gap_expected_seq) continue;
        n++;
    }
    return n;
}

static int rewinder_open(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("[udp_receiver] rewinder socket"); return -1; }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port        = 0;  // ephemeral port -- server replies to us
    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("[udp_receiver] rewinder bind");
        close(fd);
        return -1;
    }

    // Override SERVER_IP for netns testbeds where 127.0.0.1 won't cross veth.
    const char* rewinder_ip = getenv("HFT_REWINDER_IP");
    if (!rewinder_ip || !*rewinder_ip) rewinder_ip = SERVER_IP;

    memset(&gap_rewinder_addr, 0, sizeof(gap_rewinder_addr));
    gap_rewinder_addr.sin_family = AF_INET;
    gap_rewinder_addr.sin_port   = htons(RETRANSMIT_PORT);
    gap_rewinder_addr.sin_addr.s_addr = inet_addr(rewinder_ip);
    return fd;
}

// Hold a multicast membership so the kernel programs the NIC's L2 filter (and
// emits IGMP joins). Returns -1 when no group is configured.
static int mcast_membership_open(void)
{
    const char* group = getenv("HFT_FEED_MCAST_GROUP");
    if (!group || !*group) return -1;

    unsigned a = 0, b = 0, c = 0, d = 0;
    if (sscanf(group, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a < 224 || a > 239) {
        fprintf(stderr, "[udp_receiver] HFT_FEED_MCAST_GROUP='%s' not in 224/4\n", group);
        return -1;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("[udp_receiver] mcast socket"); return -1; }

    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        perror("[udp_receiver] SO_REUSEADDR");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(FEED_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[udp_receiver] mcast bind");
        close(fd);
        return -1;
    }

    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    if (inet_pton(AF_INET, group, &mreq.imr_multiaddr) != 1) {
        fprintf(stderr, "[udp_receiver] inet_pton group '%s'\n", group);
        close(fd);
        return -1;
    }
    // Pin to a specific interface; INADDR_ANY picks the wrong NIC on
    // multi-homed hosts.
    const char* iface_ip = getenv("HFT_FEED_MCAST_IFACE_IP");
    if (iface_ip && *iface_ip) {
        if (inet_pton(AF_INET, iface_ip, &mreq.imr_interface) != 1) {
            fprintf(stderr, "[udp_receiver] HFT_FEED_MCAST_IFACE_IP='%s' invalid\n", iface_ip);
            close(fd);
            return -1;
        }
    } else {
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    }
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("[udp_receiver] IP_ADD_MEMBERSHIP");
        close(fd);
        return -1;
    }

    printf("[udp_receiver] joined multicast group %s:%d (iface_ip=%s)\n",
           group, FEED_PORT, (iface_ip && *iface_ip) ? iface_ip : "<any>");
    return fd;
}

static void send_retransmit_request(uint64_t start_seq, uint16_t count, uint64_t now)
{
    if (gap_rewinder_fd < 0 || !gap_session_set) return;
    if (count == 0) return;

    // Rate-limit retransmit asks so a long gap can't flood the rewinder.
    if (now - gap_last_request_ns < RETRANSMIT_INTERVAL_NS) return;
    gap_last_request_ns = now;

    mold_udp64_request req;
    memcpy(req.session, gap_session, 10);
    req.sequence_number = htobe64(start_seq);
    req.message_count   = htobe16(count);
    sendto(gap_rewinder_fd, &req, sizeof(req), 0,
           (struct sockaddr *)&gap_rewinder_addr, sizeof(gap_rewinder_addr));
    gap_retrans_requests++;
}

// Publish a packet_ref (UMEM or reorder-backed). Returns 0 on ring-full.
static int publish_ref(packet_ref *ref)
{
    uint32_t w = atomic_load_explicit((_Atomic uint32_t *)&PARSER_ENGINE.next_write, memory_order_relaxed);
    uint32_t r = atomic_load_explicit((_Atomic uint32_t *)&PARSER_ENGINE.next_read,  memory_order_acquire);
    if (w - r >= PARSER_TO_ENGINE_SIZE) return 0;

    PARSER_ENGINE.data[w & PARSER_ENGINE_MASK] = ref;
    atomic_store_explicit((_Atomic uint32_t *)&PARSER_ENGINE.next_write, w + 1, memory_order_release);
    return 1;
}

// Publish every pending slot that has become in-order with the expected seq.
static void drain_pending(void)
{
    for (;;) {
        reorder_slot *p = reorder_find(gap_expected_seq);
        if (!p) return;
        if (!publish_ref(&p->ref)) return;  // ring full -- try next loop
        gap_expected_seq += p->msg_count;
        gap_retrans_filled++;
    }
}

// Build a packet_ref inside an existing reorder slot.
static void build_reorder_ref(reorder_slot *slot, int slot_idx, uint64_t now_ns)
{
    // data points at the ITCH message blocks (after the 20-byte MoldUDP hdr).
    slot->ref.data       = slot->bytes + sizeof(mold_udp64_header);
    slot->ref.len        = (uint32_t)slot->payload_len - (uint32_t)sizeof(mold_udp64_header);
    slot->ref.ts         = now_ns;
    slot->ref.umem_addr  = REORDER_ADDR_FLAG | (uint64_t)slot_idx;
    slot->used           = 1;
}

// Handle one MoldUDP64 packet (from AF_XDP burst or rewinder reply). Returns the
// UMEM addr to recycle, or UINT64_MAX if pinned in PARSER_ENGINE (zero-copy).
static uint64_t handle_payload(const uint8_t *payload, uint32_t payload_len,
                               uint64_t afxdp_addr, uint64_t now_ns,
                               uint8_t *umem_frame_base)
{
    if (payload_len < sizeof(mold_udp64_header))
        return afxdp_addr;

    const mold_udp64_header *hdr = (const mold_udp64_header *)payload;
    const uint16_t msg_count = be16toh(hdr->message_count);
    if (msg_count == 0) return afxdp_addr;  // heartbeat or end-of-session

    const uint64_t seq = be64toh(hdr->sequence_number);

    if (!gap_session_set) {
        memcpy(gap_session, hdr->session, 10);
        gap_session_set = 1;
        gap_expected_seq = seq;
    }

    // Late or duplicate.
    if (seq < gap_expected_seq) return afxdp_addr;

    // Fast path: in-order AF_XDP arrival, zero-copy publish via UMEM.
    if (seq == gap_expected_seq && afxdp_addr != UINT64_MAX && umem_frame_base) {
        packet_ref *ref = (packet_ref *)umem_frame_base;
        ref->data       = payload + sizeof(mold_udp64_header);
        ref->umem_addr  = afxdp_addr;
        ref->len        = payload_len - (uint32_t)sizeof(mold_udp64_header);
        ref->ts         = now_ns;
        if (!publish_ref(ref)) {
            fprintf(stderr, "[udp_receiver] PARSER ring full, dropping seq=%llu\n",
                    (unsigned long long)seq);
            gap_dropped_packets++;
            return afxdp_addr;
        }
        gap_expected_seq += msg_count;
        drain_pending();
        return UINT64_MAX;  // pinned in PARSER_ENGINE; recycle later
    }

    // In-order rewinder reply (no UMEM frame). Copy + publish.
    if (seq == gap_expected_seq) {
        // Dedupe: retransmit can race a late live arrival of the same seq.
        if (reorder_find(seq)) return afxdp_addr;
        int idx = reorder_alloc_slot();
        if (idx < 0) {
            gap_dropped_packets++;
            return afxdp_addr;
        }
        reorder_slot *slot = &reorder_pool[idx];
        slot->start_seq    = seq;
        slot->msg_count    = msg_count;
        slot->payload_len  = (uint16_t)payload_len;
        memcpy(slot->bytes, payload, payload_len);
        build_reorder_ref(slot, idx, now_ns);

        if (!publish_ref(&slot->ref)) {
            slot->used = 0;
            gap_dropped_packets++;
            return afxdp_addr;
        }
        gap_expected_seq += msg_count;
        drain_pending();
        return afxdp_addr;
    }

    // Out-of-order future packet -- buffer + ask the rewinder for the gap.
    if (reorder_find(seq)) return afxdp_addr;  // already buffered

    int idx = reorder_alloc_slot();
    if (idx < 0) {
        // Reorder pool full. Skip ahead to the next pending slot rather than
        // stall; the `> expected` guard prevents the backward-skip underflow.
        reorder_slot *next = reorder_find_min();
        if (next && next->start_seq > gap_expected_seq) {
            const uint64_t lost = next->start_seq - gap_expected_seq;
            fprintf(stderr,
                    "[udp_receiver] reorder pool full; skip-ahead from %llu to %llu (lost %llu msgs)\n",
                    (unsigned long long)gap_expected_seq,
                    (unsigned long long)next->start_seq,
                    (unsigned long long)lost);
            gap_dropped_packets += lost;
            gap_expected_seq = next->start_seq;
            drain_pending();
        } else {
            // All slots in-flight: dispatcher is back-pressuring us.
            gap_dropped_packets++;
        }
        return afxdp_addr;
    }

    reorder_slot *slot = &reorder_pool[idx];
    slot->start_seq    = seq;
    slot->msg_count    = msg_count;
    slot->payload_len  = (uint16_t)payload_len;
    memcpy(slot->bytes, payload, payload_len);
    build_reorder_ref(slot, idx, now_ns);

    // Ask the rewinder for [expected, seq).
    const uint64_t missing = seq - gap_expected_seq;
    const uint16_t cnt = (missing > 0xFFFFu) ? 0xFFFFu : (uint16_t)missing;
    send_retransmit_request(gap_expected_seq, cnt, now_ns);
    return afxdp_addr;
}

// Drain any pending rewinder responses from the kernel UDP socket. Called once
// per main-loop iteration; non-blocking.
static void poll_rewinder(uint64_t now_ns)
{
    if (gap_rewinder_fd < 0) return;
    uint8_t buf[1600];
    for (;;) {
        ssize_t n = recvfrom(gap_rewinder_fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                perror("[udp_receiver] rewinder recv");
            return;
        }
        (void)handle_payload(buf, (uint32_t)n, UINT64_MAX, now_ns, NULL);
    }
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

// Return consumed UMEM frames to AF_XDP fill ring; free reorder slots.
static int recycle_consumed_frames(struct afxdp_ctx* xsk, uint32_t* recycle_read)
{
    uint64_t addrs[RX_BURST_SIZE];
    uint32_t consumed = atomic_load_explicit((_Atomic uint32_t *)&PARSER_ENGINE.next_read, memory_order_acquire);
    int n = 0;

    while (*recycle_read != consumed) {
        packet_ref* ref = PARSER_ENGINE.data[*recycle_read & PARSER_ENGINE_MASK];
        const uint64_t addr = ref->umem_addr;
        (*recycle_read)++;

        if (addr & REORDER_ADDR_FLAG) {
            const int idx = (int)(addr & REORDER_ADDR_IDX_MASK);
            reorder_pool[idx].used = 0;
            continue;
        }

        addrs[n++] = addr;
        if (n == RX_BURST_SIZE) {
            if (afxdp_recycle(xsk, addrs, n) != 0)
                return -1;
            n = 0;
        }
    }

    if (n > 0 && afxdp_recycle(xsk, addrs, n) != 0)
        return -1;
    return 0;
}

void *udp_receiver_thread(void *arg) {
    (void)arg;

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

    atomic_store_explicit((_Atomic uint32_t *)&PARSER_ENGINE.next_write, 0, memory_order_relaxed);
    atomic_store_explicit((_Atomic uint32_t *)&PARSER_ENGINE.next_read,  0, memory_order_relaxed);

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

    // Rewinder request/response uses a kernel UDP socket: the XDP filter only
    // redirects FEED_PORT into AF_XDP.
    gap_rewinder_fd = rewinder_open();
    if (gap_rewinder_fd < 0)
        fprintf(stderr, "[udp_receiver] gap recovery disabled (rewinder_open failed)\n");

    int mcast_fd = mcast_membership_open();

    uint64_t last_stats_ns = 0;
    const uint64_t STATS_INTERVAL_NS = 2000000000ULL;  // 2 s

    // Refresh `now` only on work or every ~4k idle polls -- the tight
    // empty-loop pays zero clock_gettime cost.
    uint64_t now = 0;
    uint32_t idle_iters = 0;
    while (1) {
        if (recycle_consumed_frames(xsk, &recycle_read) != 0) {
            perror("[udp_receiver] recycle_consumed");
            break;
        }

        int n = afxdp_recv_burst(xsk, pkts, RX_BURST_SIZE);
        if (n > 0) { now = now_ns(); idle_iters = 0; }
        else if ((++idle_iters & 0xFFF) == 0) {
            now = now_ns();
        }

        if (n > 0 && last_stats_ns == 0) last_stats_ns = now;
        if (now != 0 && now - last_stats_ns >= STATS_INTERVAL_NS) {
            last_stats_ns = now;
            fprintf(stderr,
                    "[udp_receiver] expected=%llu pending=%d drops=%llu retrans_req=%llu retrans_filled=%llu\n",
                    (unsigned long long)gap_expected_seq,
                    reorder_count(),
                    (unsigned long long)gap_dropped_packets,
                    (unsigned long long)gap_retrans_requests,
                    (unsigned long long)gap_retrans_filled);
        }

        // Drain rewinder replies before live feed so an in-flight retransmit
        // can close a gap before the next live packet pushes past it.
        if (now != 0) poll_rewinder(now);
        if (n < 0) {
            perror("[udp_receiver] afxdp_recv_burst");
            break;
        }

        if (n == 0) {
            if (reorder_count() > 0 && gap_session_set) {
                // Still waiting on a gap -- keep nudging the rewinder.
                reorder_slot *next = reorder_find_min();
                if (next && next->start_seq > gap_expected_seq) {
                    const uint64_t missing = next->start_seq - gap_expected_seq;
                    const uint16_t cnt =
                        (missing > 0xFFFFu) ? 0xFFFFu : (uint16_t)missing;
                    send_retransmit_request(gap_expected_seq, cnt, now);
                }
            }
            cpu_relax();
            continue;
        }

        int stop = 0;
        uint64_t drop_addrs[RX_BURST_SIZE];
        int drop_count = 0;

        for (int i = 0; i < n; i++) {
            const uint8_t* payload;
            uint32_t payload_len;

            if (extract_feed_payload(pkts[i].data, pkts[i].len, &payload, &payload_len) != 0) {
                drop_addrs[drop_count++] = pkts[i].addr;
                continue;
            }
            if ((size_t)payload_len < sizeof(mold_udp64_header)) {
                drop_addrs[drop_count++] = pkts[i].addr;
                continue;
            }

            const mold_udp64_header* hdr = (const mold_udp64_header*) payload;
            if (be16toh(hdr->message_count) == 0xFFFF) {
                printf("[udp_receiver] end of session at seq %lu (drops=%lu retrans_req=%lu retrans_filled=%lu)\n",
                       (unsigned long)be64toh(hdr->sequence_number),
                       (unsigned long)gap_dropped_packets,
                       (unsigned long)gap_retrans_requests,
                       (unsigned long)gap_retrans_filled);
                drop_addrs[drop_count++] = pkts[i].addr;
                stop = 1;
                break;
            }

            const uint64_t recycle_addr =
                handle_payload(payload, payload_len, pkts[i].addr, now, pkts[i].data);
            if (recycle_addr != UINT64_MAX)
                drop_addrs[drop_count++] = recycle_addr;
        }

        if (drop_count > 0 && afxdp_recycle(xsk, drop_addrs, drop_count) != 0) {
            perror("[udp_receiver] recycle_drop");
            break;
        }

        if (stop)
            break;
    }

    if (gap_rewinder_fd >= 0) {
        close(gap_rewinder_fd);
        gap_rewinder_fd = -1;
    }

    if (mcast_fd >= 0) close(mcast_fd);

    // Drain remaining consumed frames before tearing down AF_XDP.
    if (recycle_consumed_frames(xsk, &recycle_read) != 0)
        perror("[udp_receiver] recycle_consumed");

    afxdp_close(xsk);
    return NULL;
}
