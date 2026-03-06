#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <endian.h>
#include "../utils/moldudp64.h"
#include "server.h"

#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif

#ifndef HFT_DEFAULT_ITCH_FILE
#define HFT_DEFAULT_ITCH_FILE "data/01302020.NASDAQ_ITCH50"
#endif

#define MAX_MSG_PAYLOAD ((int)(PACKET_SIZE - sizeof(mold_udp64_header)))

#define DELAY_BEFORE_SENDING_S 30

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

    sendto(fd, packet, sizeof(mold_udp64_header) + payload_used, 0,
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

    size_t pos = 0;
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
    bzero(&dest, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(dest_port);
    if (inet_pton(AF_INET, dest_ip, &dest.sin_addr) != 1) {
        fprintf(stderr, "[feed] invalid destination IPv4 address '%s'\n", dest_ip);
        close(fd);
        return NULL;
    }

    unsigned char session[10];
    snprintf((char*)session, sizeof(session), "%ld", time(NULL));

    if (mode != TIMESTAMP_MODE) {
        printf("[feed] sending to %s:%d in %d seconds...\n",
               dest_ip, dest_port, DELAY_BEFORE_SENDING_S);
        sleep(DELAY_BEFORE_SENDING_S);
    }
    printf("[feed] ITCH source: %s\n", itch_path);
    printf("[feed] starting (mode %d).\n", mode);

    switch (mode) {
        case NORMAL_MODE:
            printf("[feed] sequential\n");
            bulk_feed(fd, &dest, session, 0, 0, itch_path);
            break;
        case LOSSY_MODE:
            printf("[feed] lossy — dropping every 100th packet\n");
            bulk_feed(fd, &dest, session, 100, 0, itch_path);
            break;
        case CHAOTIC_MODE:
            printf("[feed] chaotic — duplicating every 10th packet\n");
            bulk_feed(fd, &dest, session, 0, 10, itch_path);
            break;
        case TIMESTAMP_MODE:
            printf("[feed] timestamp replay — %.2fx speed\n", cfg->speed);
            timestamp_feed(fd, &dest, session, itch_path, cfg->speed);
            break;
        default:
            fprintf(stderr, "[feed] unknown mode %d\n", mode);
            break;
    }

    close(fd);
    return NULL;
}
