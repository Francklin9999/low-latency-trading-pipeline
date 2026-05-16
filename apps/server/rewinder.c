#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
// MoldUDP64 retransmit cache; feed-thread writes a ring, rewinder serves replays.
// SPSC: writer zeroes start_seq before mutating so readers treat overwrites as misses.

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hft/server/server.h"
#include "hft/utils/moldudp64.h"

#define REWINDER_CACHE_SLOTS (64u * 1024u)   // ~ several seconds at our rate
#define REWINDER_CACHE_MASK  (REWINDER_CACHE_SLOTS - 1u)
#define REWINDER_MAX_PACKET  1500
#define REWINDER_MAX_REPLAY  16              // per request, before we give up

typedef struct {
    _Atomic uint64_t start_seq;     // 0 == empty
    uint16_t         msg_count;
    uint16_t         len;
    unsigned char    session[10];
    unsigned char    bytes[REWINDER_MAX_PACKET];
} cached_packet;

static cached_packet  g_cache[REWINDER_CACHE_SLOTS];
static _Atomic uint32_t g_write_cursor = 0;

void rewinder_init(void)
{
    memset(g_cache, 0, sizeof(g_cache));
    atomic_store_explicit(&g_write_cursor, 0, memory_order_relaxed);
}

void rewinder_cache_packet(unsigned char session[10],
                           uint64_t start_seq,
                           uint16_t msg_count,
                           const void *packet_bytes,
                           uint16_t packet_len)
{
    if (packet_len > REWINDER_MAX_PACKET || msg_count == 0)
        return;

    const uint32_t w = atomic_load_explicit(&g_write_cursor, memory_order_relaxed);
    cached_packet *slot = &g_cache[w & REWINDER_CACHE_MASK];

    // Mark slot empty before mutating; see SPSC note at top of file.
    atomic_store_explicit(&slot->start_seq, 0, memory_order_relaxed);
    memcpy(slot->session, session, 10);
    slot->msg_count = msg_count;
    slot->len       = packet_len;
    memcpy(slot->bytes, packet_bytes, packet_len);
    atomic_store_explicit(&slot->start_seq, start_seq, memory_order_release);

    atomic_store_explicit(&g_write_cursor, w + 1u, memory_order_release);
}

// Find the slot covering `seq`. Scans backward from the write cursor:
// near-tail requests hit in 1-2 probes; REWINDER_MAX_REPLAY caps work upstream.
static const cached_packet *find_packet(uint64_t seq)
{
    const uint32_t w = atomic_load_explicit(&g_write_cursor, memory_order_acquire);
    const uint32_t scan_max =
        (w < REWINDER_CACHE_SLOTS) ? w : REWINDER_CACHE_SLOTS;

    for (uint32_t i = 1; i <= scan_max; i++) {
        const cached_packet *slot = &g_cache[(w - i) & REWINDER_CACHE_MASK];
        const uint64_t s = atomic_load_explicit(&slot->start_seq,
                                                memory_order_acquire);
        if (s == 0) continue;
        const uint64_t end = s + (uint64_t)slot->msg_count;
        if (s <= seq && seq < end) return slot;
    }
    return NULL;
}

void *rewinder_thread(void *arg)
{
    (void)arg;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("[rewinder] socket"); return NULL; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = inet_addr(BIND_IP);
    bind_addr.sin_port        = htons(RETRANSMIT_PORT);

    if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("[rewinder] bind");
        close(fd);
        return NULL;
    }

    printf("[rewinder] listening on %s:%d (UDP)\n", BIND_IP, RETRANSMIT_PORT);

    for (;;) {
        mold_udp64_request req;
        struct sockaddr_in cli;
        socklen_t cli_len = sizeof(cli);

        ssize_t n = recvfrom(fd, &req, sizeof(req), 0,
                             (struct sockaddr *)&cli, &cli_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[rewinder] recvfrom");
            break;
        }
        if (n != (ssize_t)sizeof(req)) continue;

        uint64_t want_seq = be64toh(req.sequence_number);
        uint16_t want_cnt = be16toh(req.message_count);
        if (want_cnt == 0) continue;

        uint16_t replayed = 0;
        for (uint16_t i = 0; i < REWINDER_MAX_REPLAY && want_cnt > 0; i++) {
            const cached_packet *p = find_packet(want_seq);
            if (!p) break;

            sendto(fd, p->bytes, p->len, 0,
                   (struct sockaddr *)&cli, cli_len);
            replayed++;

            const uint64_t next_seq = atomic_load_explicit(&p->start_seq,
                                                           memory_order_acquire)
                                    + (uint64_t)p->msg_count;
            if (next_seq <= want_seq) break;  // defensive: no progress
            const uint64_t covered = next_seq - want_seq;
            want_seq = next_seq;
            want_cnt = (covered >= want_cnt) ? 0 : (uint16_t)(want_cnt - covered);
        }

        if (replayed == 0) {
            fprintf(stderr,
                    "[rewinder] no cache for seq=%llu count=%u (out of window?)\n",
                    (unsigned long long)be64toh(req.sequence_number),
                    (unsigned)be16toh(req.message_count));
        }
    }

    close(fd);
    return NULL;
}
