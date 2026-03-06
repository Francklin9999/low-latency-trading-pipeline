#pragma once

#include <stdint.h>

#define BIND_IP "127.0.0.1"
#define FEED_PORT 5000 // UDP
#define ORDER_PORT  5001 // TCP

#define NORMAL_MODE 1
#define LOSSY_MODE 2
#define CHAOTIC_MODE 3
#define TIMESTAMP_MODE 4

typedef struct {
    int mode;
    const char* dest_ip;
    uint16_t dest_port;
    const char* itch_path;
    double speed;   // replay speed multiplier: 1.0=realtime, 2.0=2x faster
} feed_config_t;

void *feed_thread(void *arg);
void *order_receiver_thread(void *arg);
