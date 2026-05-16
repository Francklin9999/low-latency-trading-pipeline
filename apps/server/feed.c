#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <endian.h>
#include "hft/utils/moldudp64.h"
#include "hft/server/server.h"

#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

#ifndef HFT_DEFAULT_ITCH_FILE
#define HFT_DEFAULT_ITCH_FILE "data/01302020.NASDAQ_ITCH50"
#endif

#define MAX_MSG_PAYLOAD ((int)(PACKET_SIZE - sizeof(mold_udp64_header)))

#define DELAY_BEFORE_SENDING_S_DEFAULT 30

// Skip the ~440 MB of pre-market admin in the bundled 2020-01-30 file so a 1.0x
// run hits trading load immediately. Other ITCH files replay from byte 0.
#define ITCH_BUNDLED_FILE_SIZE     12952050754ULL  // exact size of 01302020.NASDAQ_ITCH50
#define ITCH_BUNDLED_OPEN_OFFSET   440787451ULL    // first msg ts >= 09:30:00.000 ET

static size_t initial_file_offset(off_t file_size) {
    if ((uint64_t)file_size == ITCH_BUNDLED_FILE_SIZE) {
        printf("[feed] bundled 2020-01-30 ITCH detected -- skipping pre-market admin\n");
        printf("[feed] starting at byte %llu (09:30:00 ET cash open)\n",
               (unsigned long long)ITCH_BUNDLED_OPEN_OFFSET);
        return (size_t)ITCH_BUNDLED_OPEN_OFFSET;
    }
    printf("[feed] non-bundled ITCH file (size %lld) -- replaying from byte 0\n",
           (long long)file_size);
    return 0;
}

// HFT_FEED_DELAY_S overrides the 30s start delay (e.g. for smoke tests).
static unsigned feed_delay_s(void) {
    const char *v = getenv("HFT_FEED_DELAY_S");
    if (!v || !*v) return DELAY_BEFORE_SENDING_S_DEFAULT;
    char *end = NULL;
    unsigned long u = strtoul(v, &end, 10);
    if (*end != '\0') return DELAY_BEFORE_SENDING_S_DEFAULT;
    return (unsigned)u;
}

static int ipv4_is_multicast(const char* ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!ip) return 0;
    if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    return a >= 224 && a <= 239;
}

// Configure multicast TX. iface_ip pins the source NIC; ttl=1 keeps the frame
// on the local segment; IP_MULTICAST_LOOP is set explicitly for loopback.
static int configure_multicast(int fd, const char* iface_ip, int ttl) {
    int one = 1;
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &one, sizeof(one)) < 0) {
        perror("[feed] IP_MULTICAST_LOOP");
        return -1;
    }
    unsigned char ttl_u = (unsigned char)ttl;
    if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl_u, sizeof(ttl_u)) < 0) {
        perror("[feed] IP_MULTICAST_TTL");
        return -1;
    }
    if (iface_ip && iface_ip[0] != '\0') {
        struct in_addr local;
        if (inet_pton(AF_INET, iface_ip, &local) != 1) {
            fprintf(stderr, "[feed] HFT_FEED_MCAST_IFACE_IP='%s' invalid\n", iface_ip);
            return -1;
        }
        if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &local, sizeof(local)) < 0) {
            perror("[feed] IP_MULTICAST_IF");
            return -1;
        }
    }
    return 0;
}

static inline uint64_t msg_timestamp(const uint8_t* msg) {
    return ((uint64_t)msg[5] << 40) |
           ((uint64_t)msg[6] << 32) |
           ((uint64_t)msg[7] << 24) |
           ((uint64_t)msg[8] << 16) |
           ((uint64_t)msg[9] << 8) |
            (uint64_t)msg[10];
}

static void send_end_of_session(int fd, struct sockaddr_in* dest,
                                unsigned char session[10], uint64_t seq) {
    char packet[sizeof(mold_udp64_header)];
    mold_udp64_header* hdr = (mold_udp64_header*) packet;
    memcpy(hdr->session, session, 10);
    hdr->sequence_number = htobe64(seq);
    hdr->message_count = htobe16(0xFFFF);
    sendto(fd, packet, sizeof(mold_udp64_header), 0,
           (struct sockaddr*) dest, sizeof(*dest));
}

static void bulk_feed(int fd, struct sockaddr_in* dest,
                      unsigned char session[10],
                      uint64_t drop_every, uint64_t dup_every,
                      const char *itch_path) {

    int data_fd = open(itch_path, O_RDONLY);
    if (data_fd < 0) {
        fprintf(stderr, "[feed] cannot open ITCH file '%s': ", itch_path);
        perror("");
        return;
    }

    struct stat st_skip;
    if (fstat(data_fd, &st_skip) == 0) {
        const size_t skip = initial_file_offset(st_skip.st_size);
        if (skip > 0 && lseek(data_fd, (off_t)skip, SEEK_SET) < 0) {
            perror("[feed] lseek to market-open offset");
            close(data_fd);
            return;
        }
    }

    char itch_buf[65536];
    char packet[PACKET_SIZE];
    uint64_t seq = 1;
    int leftover = 0;
    ssize_t n;

    while ((n = read(data_fd, itch_buf + leftover,
                     sizeof(itch_buf) - leftover)) > 0) {
        n += leftover;
        int offset = 0;

        while (1) {
            if (offset + 2 > n) break;

            mold_udp64_header* hdr = (mold_udp64_header*) packet;
            char* payload = packet + sizeof(mold_udp64_header);
            int payload_used = 0;
            uint16_t msg_count = 0;

            while (1) {
                if (offset + 2 > n) break;
                uint16_t msg_size = be16toh(*(uint16_t*) &itch_buf[offset]);
                if (offset + 2 + (int)msg_size > n) break;
                if (payload_used + 2 + (int)msg_size > MAX_MSG_PAYLOAD) break;

                uint16_t net_size = htobe16(msg_size);
                memcpy(payload + payload_used, &net_size, 2);
                payload_used += 2;
                memcpy(payload + payload_used, &itch_buf[offset + 2], msg_size);
                payload_used += msg_size;
                msg_count++;
                offset += 2 + msg_size;
            }

            if (msg_count == 0) break;

            memcpy(hdr->session, session, 10);
            hdr->sequence_number = htobe64(seq);
            hdr->message_count = htobe16(msg_count);

            int pkt_size = sizeof(mold_udp64_header) + payload_used;

            // Populate the rewinder cache for every packet we would have sent,
            // including dropped ones, so the client can recover via retransmit.
            rewinder_cache_packet(session, seq, msg_count, packet, pkt_size);

            if (drop_every == 0 || (seq % drop_every) != 0) {
                sendto(fd, packet, pkt_size, 0,
                       (struct sockaddr*) dest, sizeof(*dest));
                if (dup_every > 0 && (seq % dup_every) == 0)
                    sendto(fd, packet, pkt_size, 0,
                           (struct sockaddr*) dest, sizeof(*dest));
            }

            seq += msg_count;
        }

        leftover = n - offset;
        memmove(itch_buf, itch_buf + offset, leftover);
    }

    send_end_of_session(fd, dest, session, seq);
    printf("[feed] done. Last sequence %lu.\n", seq - 1);
    close(data_fd);
}

static void flush_packet(int fd, struct sockaddr_in* dest,
                         char* packet, int payload_used, uint16_t msg_count,
                         unsigned char session[10], uint64_t seq,
                         const struct timespec *target) {
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, target, NULL);
    } while (rc == EINTR);

    mold_udp64_header* hdr = (mold_udp64_header*) packet;
    memcpy(hdr->session, session, 10);
    hdr->sequence_number = htobe64(seq);
    hdr->message_count = htobe16(msg_count);

    const int pkt_size = (int)sizeof(mold_udp64_header) + payload_used;
    rewinder_cache_packet(session, seq, msg_count, packet, (uint16_t)pkt_size);
    sendto(fd, packet, (size_t)pkt_size, 0,
           (struct sockaddr*) dest, sizeof(*dest));
}

static void timestamp_feed(int fd, struct sockaddr_in* dest,
                           unsigned char session[10],
                           const char *itch_path,
                           double speed) {
    if (speed <= 0.0) {
        fprintf(stderr, "[feed] invalid speed %.6f\n", speed);
        return;
    }

    int data_fd = open(itch_path, O_RDONLY);
    if (data_fd < 0) {
        fprintf(stderr, "[feed] cannot open ITCH file '%s': ", itch_path);
        perror("");
        return;
    }

    struct stat st;
    if (fstat(data_fd, &st) < 0) {
        perror("[feed] fstat");
        close(data_fd);
        return;
    }

    uint8_t* data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, data_fd, 0);
    close(data_fd);
    if (data == MAP_FAILED) {
        perror("[feed] mmap");
        return;
    }

    madvise(data, st.st_size, MADV_SEQUENTIAL);

    char packet[PACKET_SIZE];
    char* payload = packet + sizeof(mold_udp64_header);

    uint64_t seq = 1;
    int payload_used = 0;
    uint16_t msg_count = 0;

    struct timespec replay_start = {0, 0};
    uint64_t first_packet_ts = 0;
    uint64_t current_packet_ts = 0;
    int replay_started = 0;

    size_t pos = initial_file_offset(st.st_size);
    size_t file_size = (size_t)st.st_size;

    while (pos + 2 <= file_size) {
        uint16_t msg_size;
        memcpy(&msg_size, data + pos, sizeof(msg_size));
        msg_size = be16toh(msg_size);
        pos += 2;

        if (pos + msg_size > file_size) break;

        uint8_t *msg = data + pos;
        pos += msg_size;

        if (msg_size < 11) continue;

        uint64_t itch_ts = msg_timestamp(msg);
        int would_overflow = (payload_used + 2 + (int)msg_size > MAX_MSG_PAYLOAD);

        if (msg_count == 0) {
            current_packet_ts = itch_ts;

            if (!replay_started) {
                clock_gettime(CLOCK_MONOTONIC, &replay_start);
                first_packet_ts = current_packet_ts;
                replay_started = 1;
            }
        }

        if (msg_count > 0 && (itch_ts != current_packet_ts || would_overflow)) {
            uint64_t delta_ns =
                (uint64_t)((double)(current_packet_ts - first_packet_ts) / speed);

            struct timespec target = replay_start;
            target.tv_sec  += (time_t)(delta_ns / 1000000000ULL);
            target.tv_nsec += (long)(delta_ns % 1000000000ULL);

            if (target.tv_nsec >= 1000000000L) {
                target.tv_sec++;
                target.tv_nsec -= 1000000000L;
            }

            flush_packet(fd, dest, packet, payload_used, msg_count,
                         session, seq, &target);

            seq += msg_count;
            payload_used = 0;
            msg_count = 0;
            current_packet_ts = itch_ts;
        }

        {
            uint16_t net_size = htobe16(msg_size);
            memcpy(payload + payload_used, &net_size, 2);
            payload_used += 2;

            memcpy(payload + payload_used, msg, msg_size);
            payload_used += msg_size;

            msg_count++;
        }
    }

    if (msg_count > 0) {
        uint64_t delta_ns =
            (uint64_t)((double)(current_packet_ts - first_packet_ts) / speed);

        struct timespec target = replay_start;
        target.tv_sec  += (time_t)(delta_ns / 1000000000ULL);
        target.tv_nsec += (long)(delta_ns % 1000000000ULL);

        if (target.tv_nsec >= 1000000000L) {
            target.tv_sec++;
            target.tv_nsec -= 1000000000L;
        }

        flush_packet(fd, dest, packet, payload_used, msg_count,
                     session, seq, &target);

        seq += msg_count;
    }

    send_end_of_session(fd, dest, session, seq);
    printf("[feed] done. Last sequence %lu.\n", seq - 1);

    munmap(data, st.st_size);
}

void *feed_thread(void* arg) {
    const feed_config_t* cfg = (const feed_config_t*) arg;
    const int mode = cfg->mode;
    const char* dest_ip = (cfg->dest_ip != NULL && cfg->dest_ip[0] != '\0')
                              ? cfg->dest_ip
                              : BIND_IP;
    const uint16_t dest_port = (cfg->dest_port != 0) ? cfg->dest_port : FEED_PORT;
    const char* itch_path = (cfg->itch_path != NULL && cfg->itch_path[0] != '\0')
                                ? cfg->itch_path
                                : HFT_DEFAULT_ITCH_FILE;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("[feed] socket"); return NULL; }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(dest_port);
    if (inet_pton(AF_INET, dest_ip, &dest.sin_addr) != 1) {
        fprintf(stderr, "[feed] invalid destination IPv4 address '%s'\n", dest_ip);
        close(fd);
        return NULL;
    }

    const int multicast = ipv4_is_multicast(dest_ip);
    if (multicast) {
        const char* iface_ip = getenv("HFT_FEED_MCAST_IFACE_IP");
        if (configure_multicast(fd, iface_ip, 1) != 0) {
            close(fd);
            return NULL;
        }
        printf("[feed] multicast mode (group=%s, iface_ip=%s, ttl=1)\n",
               dest_ip, (iface_ip && iface_ip[0]) ? iface_ip : "<routed>");
    } else {
        printf("[feed] unicast mode (dest=%s)\n", dest_ip);
    }

    unsigned char session[10];
    snprintf((char*)session, sizeof(session), "%ld", time(NULL));

    if (mode != TIMESTAMP_MODE) {
        const unsigned d = feed_delay_s();
        printf("[feed] sending to %s:%d in %u seconds...\n",
               dest_ip, dest_port, d);
        if (d > 0) sleep(d);
    }
    printf("[feed] ITCH source: %s\n", itch_path);
    printf("[feed] starting (mode %d).\n", mode);

    switch (mode) {
        case NORMAL_MODE:
            printf("[feed] sequential\n");
            bulk_feed(fd, &dest, session, 0, 0, itch_path);
            break;
        case LOSSY_MODE:
            printf("[feed] lossy -- dropping every 100th packet\n");
            bulk_feed(fd, &dest, session, 100, 0, itch_path);
            break;
        case CHAOTIC_MODE:
            printf("[feed] chaotic -- duplicating every 10th packet\n");
            bulk_feed(fd, &dest, session, 0, 10, itch_path);
            break;
        case TIMESTAMP_MODE:
            printf("[feed] timestamp replay -- %.2fx speed\n", cfg->speed);
            timestamp_feed(fd, &dest, session, itch_path, cfg->speed);
            break;
        default:
            fprintf(stderr, "[feed] unknown mode %d\n", mode);
            break;
    }

    close(fd);
    return NULL;
}
