#pragma once

#include <stdint.h>

#define BIND_IP "127.0.0.1"
#define FEED_MCAST_GROUP "239.1.1.1"
#define FEED_PORT 5000
#define ORDER_PORT 5001
#define RETRANSMIT_PORT 5002

#define NORMAL_MODE 1
#define LOSSY_MODE 2
#define CHAOTIC_MODE 3
#define TIMESTAMP_MODE 4

typedef struct {
    int mode;
    const char* dest_ip;
    uint16_t dest_port;
    const char* itch_path;
    double speed;
} feed_config_t;

void *feed_thread(void *arg);
void *order_receiver_thread(void *arg);
void *rewinder_thread(void *arg);

void rewinder_init(void);
void rewinder_cache_packet(unsigned char session[10],
                           uint64_t start_seq,
                           uint16_t msg_count,
                           const void* packet_bytes,
                           uint16_t packet_len);
