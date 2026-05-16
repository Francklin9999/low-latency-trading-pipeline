#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "hft/client/client.h"
#include "hft/utils/utils.h"
#include "hft/protocol/wire.h"
#include "hft/afxdp/afxdp_tx.h"

#define RECONNECT_DELAY_S 1
#define ACK_BUF_BYTES     (16 * (int)sizeof(wire_ack))
#define ACK_POLL_MASK     63u

static uint64_t sender_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Latency sampler: one store per send into a single-writer ring; sort-and-emit
// p50/p90/p99/max once per ~65k orders, off the critical path.
#define LAT_RING_SZ 4096u
#define LAT_RING_MASK (LAT_RING_SZ - 1u)
static uint32_t lat_ring[LAT_RING_SZ];
static uint32_t lat_count = 0;          // total samples ever written
static uint32_t lat_window_start = 0;   // count at last print

static int u32_cmp(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

// Snapshot for telemetry. SWSR: torn reads are statistically harmless.
void order_sender_lat_snapshot_ns(uint32_t* p50, uint32_t* p90,
                                  uint32_t* p99, uint32_t* mx,
                                  uint32_t* n_out) {
    *p50 = *p90 = *p99 = *mx = 0;
    *n_out = 0;
    const uint32_t n_total = lat_count;
    if (n_total == 0) return;
    const uint32_t n = (n_total < LAT_RING_SZ) ? n_total : LAT_RING_SZ;
    uint32_t buf[LAT_RING_SZ];
    for (uint32_t i = 0; i < n; ++i) {
        buf[i] = lat_ring[(lat_count - n + i) & LAT_RING_MASK];
    }
    qsort(buf, n, sizeof(buf[0]), u32_cmp);
    *p50 = buf[(n * 50) / 100];
    *p90 = buf[(n * 90) / 100];
    *p99 = buf[(n >= 100) ? (n * 99) / 100 : n - 1];
    *mx  = buf[n - 1];
    *n_out = n;
}

static void lat_print_and_reset(const char* via) {
    const uint32_t n_total = lat_count - lat_window_start;
    if (n_total == 0) return;
    const uint32_t n = (n_total < LAT_RING_SZ) ? n_total : LAT_RING_SZ;
    uint32_t buf[LAT_RING_SZ];
    // the last n samples sit at indices [lat_count-n .. lat_count) mod size
    for (uint32_t i = 0; i < n; ++i) {
        buf[i] = lat_ring[(lat_count - n + i) & LAT_RING_MASK];
    }
    qsort(buf, n, sizeof(buf[0]), u32_cmp);
    const uint32_t p50 = buf[(n * 50) / 100];
    const uint32_t p90 = buf[(n * 90) / 100];
    const uint32_t p99 = buf[(n >= 100) ? (n * 99) / 100 : n - 1];
    const uint32_t mx  = buf[n - 1];
    printf("[order_sender] lat_parse_to_send_ns p50=%u p90=%u p99=%u max=%u "
           "(n=%u, via=%s)\n", p50, p90, p99, mx, n, via);
    lat_window_start = lat_count;
}

static int connect_to_exchange(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("[order_sender] socket"); return -1; }

    // Disable Nagle -- every order must be flushed immediately, not batched
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ORDER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    while (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[order_sender] connect, retrying...");
        sleep(RECONNECT_DELAY_S);
    }

    // Non-blocking: send() returns EAGAIN instead of sleeping this thread
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    printf("[order_sender] connected to %s:%d\n", SERVER_IP, ORDER_PORT);
    return fd;
}

// Non-blocking ACK drain; never stalls the send path.
static void drain_acks(int fd, uint64_t *acked_seq,
                       uint64_t *bad_csum_count, uint64_t *seq_gap_count)
{
    uint8_t  buf[ACK_BUF_BYTES];
    static size_t carry = 0;
    static uint8_t carry_buf[sizeof(wire_ack)];

    // Carry-over from a previous partial recv.
    if (carry > 0) {
        memcpy(buf, carry_buf, carry);
    }

    ssize_t n = recv(fd, buf + carry, sizeof(buf) - carry, MSG_DONTWAIT);
    if (n <= 0) {
        return;
    }

    size_t total = carry + (size_t)n;
    size_t off = 0;
    while (off + sizeof(wire_ack) <= total) {
        const wire_ack *a = (const wire_ack *)(buf + off);
        if (a->msg_type == WIRE_MSG_ACK) {
            if (a->seq > *acked_seq) *acked_seq = a->seq;
            if (a->flag == WIRE_ACK_BAD_CSUM) (*bad_csum_count)++;
            else if (a->flag == WIRE_ACK_SEQ_GAP) (*seq_gap_count)++;
        }
        off += sizeof(wire_ack);
    }

    carry = total - off;
    if (carry > 0) memcpy(carry_buf, buf + off, carry);
}

// HFT_AFXDP_TX=1 swaps order egress from TCP to AF_XDP TX.
static int afxdp_tx_enabled(void) {
    const char *v = getenv("HFT_AFXDP_TX");
    return v && v[0] != '\0' && strcmp(v, "0") != 0;
}

static void parse_mac(const char *s, uint8_t mac[6]) {
    if (!s) { memset(mac, 0, 6); return; }
    unsigned int b[6] = {0};
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (int i = 0; i < 6; i++) mac[i] = (uint8_t)b[i];
    } else {
        memset(mac, 0, 6);
    }
}

static struct afxdp_tx_ctx *init_afxdp_tx_from_env(void) {
    struct afxdp_tx_config tx;
    memset(&tx, 0, sizeof(tx));
    tx.ifname        = getenv("HFT_AFXDP_TX_IFACE");
    if (!tx.ifname || tx.ifname[0] == '\0') tx.ifname = "lo";
    const char *qs   = getenv("HFT_AFXDP_TX_QUEUE");
    tx.queue_id      = qs ? (uint32_t)atoi(qs) : 0;
    tx.src_ip_be     = inet_addr(SERVER_IP);  // loopback both ways
    tx.dst_ip_be     = inet_addr(SERVER_IP);
    tx.src_port      = 49152;
    tx.dst_port      = ORDER_PORT;
    parse_mac(getenv("HFT_AFXDP_TX_SRC_MAC"), tx.src_mac);
    parse_mac(getenv("HFT_AFXDP_TX_DST_MAC"), tx.dst_mac);
    tx.force_zerocopy = (getenv("HFT_AFXDP_TX_ZEROCOPY") != NULL);
    tx.use_hugepages  = 1;

    struct afxdp_tx_ctx *ctx = NULL;
    if (afxdp_tx_init(&ctx, &tx) != 0) {
        fprintf(stderr, "[order_sender] afxdp_tx_init failed: %s -- falling back to TCP\n",
                strerror(errno));
        return NULL;
    }
    return ctx;
}

void *order_sender_thread(void *arg) {
    (void)arg;

    struct afxdp_tx_ctx *tx_ctx = NULL;
    int fd = -1;
    int via_afxdp = 0;

    if (afxdp_tx_enabled()) {
        tx_ctx = init_afxdp_tx_from_env();
        via_afxdp = (tx_ctx != NULL);
    }
    if (!via_afxdp) {
        fd = connect_to_exchange();
        if (fd < 0) return NULL;
    } else {
        printf("[order_sender] AF_XDP TX active; TCP path disabled this session\n");
    }

    // Per-session seq; resets on reconnect (server resets expected_seq on accept).
    uint64_t next_seq       = 1;
    uint64_t acked_seq      = 0;
    uint64_t bad_csum_count = 0;
    uint64_t seq_gap_count  = 0;
    uint32_t ack_poll_ticks = 0;

    while (1) {
        uint32_t r = atomic_load_explicit((_Atomic uint32_t *)&ORDER_TO_EXC->next_read,  memory_order_relaxed);
        uint32_t w = atomic_load_explicit((_Atomic uint32_t *)&ORDER_TO_EXC->next_write, memory_order_acquire);

        if (r == w) {
            if (!via_afxdp && ((++ack_poll_ticks & ACK_POLL_MASK) == 0u)) {
                drain_acks(fd, &acked_seq, &bad_csum_count, &seq_gap_count);
            }
            if (via_afxdp) afxdp_tx_drain_completions(tx_ctx);
            cpu_relax();
            continue;
        }

        if (!via_afxdp && ((++ack_poll_ticks & ACK_POLL_MASK) == 0u)) {
            drain_acks(fd, &acked_seq, &bad_csum_count, &seq_gap_count);
        }

        order* slot = &ORDER_TO_EXC->data[r & ORDER_TO_EXC_MASK];
        const uint64_t send_ns = sender_now_ns();
        slot->send_ns = send_ns;

        // one store, no atomics -- hot-path cost is a single 4-byte write
        const uint64_t parse_ns = slot->parse_ns;
        const uint32_t delta = (parse_ns != 0 && send_ns >= parse_ns)
            ? (uint32_t)((send_ns - parse_ns) > UINT32_MAX
                            ? UINT32_MAX
                            : (send_ns - parse_ns))
            : 0u;
        lat_ring[lat_count & LAT_RING_MASK] = delta;
        ++lat_count;

        wire_order msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_type  = (slot->qty == 0) ? WIRE_MSG_CANCEL : WIRE_MSG_NEW;
        msg.side      = slot->side;
        msg.symbol_id = slot->stock_locate;
        msg.qty       = slot->qty;
        msg.price     = slot->price;
        msg.seq       = next_seq;
        msg.ts        = send_ns;
        msg.checksum  = 0;
        msg.checksum  = wire_checksum(&msg, sizeof(msg) - sizeof(msg.checksum) - sizeof(msg._pad1));

        if (via_afxdp) {
            // Backpressure surfaces as EAGAIN/ENOSPC; spin and retry.
            for (;;) {
                int rc = afxdp_tx_send(tx_ctx, &msg, sizeof(msg));
                if (rc == 0) break;
                if (errno == EAGAIN || errno == ENOSPC) {
                    afxdp_tx_drain_completions(tx_ctx);
                    cpu_relax();
                    continue;
                }
                fprintf(stderr, "[order_sender] afxdp_tx_send fatal: %s\n",
                        strerror(errno));
                goto done;
            }
        } else {
            const char* buf = (const char*)&msg;
            size_t      remaining = sizeof(msg);
            while (remaining > 0) {
                ssize_t n = send(fd, buf, remaining, MSG_NOSIGNAL);
                if (n > 0) {
                    buf       += n;
                    remaining -= (size_t)n;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    drain_acks(fd, &acked_seq, &bad_csum_count, &seq_gap_count);
                    cpu_relax();
                } else {
                    perror("[order_sender] send, reconnecting...");
                    close(fd);
                    fd = connect_to_exchange();
                    if (fd < 0) goto done;
                    next_seq   = 1;
                    acked_seq  = 0;
                    buf        = (const char*)&msg;
                    remaining  = sizeof(msg);
                    msg.seq    = next_seq;
                    msg.checksum = 0;
                    msg.checksum = wire_checksum(&msg, sizeof(msg) - sizeof(msg.checksum) - sizeof(msg._pad1));
                }
            }
        }

        next_seq++;
        atomic_store_explicit((_Atomic uint32_t *)&ORDER_TO_EXC->next_read, r + 1, memory_order_release);

        if ((next_seq & 0xFFFFu) == 0u) {
            printf("[order_sender] sent=%lu acked=%lu bad_csum=%lu seq_gap=%lu (via=%s)\n",
                   (unsigned long)(next_seq - 1),
                   (unsigned long)acked_seq,
                   (unsigned long)bad_csum_count,
                   (unsigned long)seq_gap_count,
                   via_afxdp ? "afxdp" : "tcp");
            lat_print_and_reset(via_afxdp ? "afxdp" : "tcp");
        }
    }

    done:
        if (fd >= 0) close(fd);
        if (tx_ctx) afxdp_tx_close(tx_ctx);
        return NULL;
}
