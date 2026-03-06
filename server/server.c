#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "server.h"

#ifdef __linux__
#include <sched.h>
#include <errno.h>
#endif

#ifndef HFT_SERVER_FEED_CORE
#define HFT_SERVER_FEED_CORE 2
#endif
#ifndef HFT_SERVER_ORDER_RECV_CORE
#define HFT_SERVER_ORDER_RECV_CORE 3
#endif

static int parse_feed_port(const char* s, uint16_t* out_port) {
    char* end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 1 || v > 65535) {
        return -1;
    }
    *out_port = (uint16_t)v;
    return 0;
}

#ifdef __linux__
static int pin_attr_to_core(pthread_attr_t *attr, int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET((size_t)core, &set);
    int rc = pthread_attr_setaffinity_np(attr, sizeof(set), &set);
    if (rc != 0) {
        fprintf(stderr, "pthread_attr_setaffinity_np(core=%d) failed: %s\n",
                core, strerror(rc));
        return -1;
    }
    return 0;
}
#endif

int main(int argc, char** argv) {
    if (argc < 2 || argc > 6) {
        fprintf(stderr,
                "Usage: %s <mode: 1=normal 2=lossy 3=chaotic 4=timestamp> "
                "[feed_port] [feed_ip] [itch_file] [speed]\n",
                argv[0]);
        return 1;
    }

    int mode = atoi(argv[1]);
    if (mode < NORMAL_MODE || mode > TIMESTAMP_MODE) {
        fprintf(stderr, "Unknown mode %d\n", mode);
        return 1;
    }

    feed_config_t feed_cfg = {
        .mode = mode,
        .dest_ip = BIND_IP,
        .dest_port = FEED_PORT,
        .itch_path = NULL,
        .speed = 1.0,
    };

    if (argc >= 3 && parse_feed_port(argv[2], &feed_cfg.dest_port) != 0) {
        fprintf(stderr, "Invalid feed_port '%s' (expected 1..65535)\n", argv[2]);
        return 1;
    }
    if (argc >= 4) {
        feed_cfg.dest_ip = argv[3];
    }
    if (argc >= 5) {
        feed_cfg.itch_path = argv[4];
    }
    if (argc >= 6) {
        char* end = NULL;
        double s = strtod(argv[5], &end);
        if (end == argv[5] || s <= 0.0) {
            fprintf(stderr, "Invalid speed '%s' (expected positive number, e.g. 1 2 0.5)\n", argv[5]);
            return 1;
        }
        feed_cfg.speed = s;
    }

    printf("[server] mode=%d feed=%s:%u\n", feed_cfg.mode, feed_cfg.dest_ip, feed_cfg.dest_port);
    if (feed_cfg.itch_path != NULL) {
        printf("[server] ITCH file override: %s\n", feed_cfg.itch_path);
    }

    pthread_t feed_tid, order_tid;
    pthread_attr_t feed_attr, order_attr;
    pthread_attr_init(&feed_attr);
    pthread_attr_init(&order_attr);

#ifdef __linux__
    if (pin_attr_to_core(&feed_attr,  HFT_SERVER_FEED_CORE)       != 0) return 1;
    if (pin_attr_to_core(&order_attr, HFT_SERVER_ORDER_RECV_CORE)  != 0) return 1;
    printf("[server] feed thread  -> core %d\n", HFT_SERVER_FEED_CORE);
    printf("[server] order thread -> core %d\n", HFT_SERVER_ORDER_RECV_CORE);
#endif

    if (pthread_create(&order_tid, &order_attr, order_receiver_thread, NULL) != 0) {
        perror("pthread_create order_receiver");
        return 1;
    }

    if (pthread_create(&feed_tid, &feed_attr, feed_thread, &feed_cfg) != 0) {
        perror("pthread_create feed");
        return 1;
    }

    pthread_attr_destroy(&feed_attr);
    pthread_attr_destroy(&order_attr);

    pthread_join(feed_tid, NULL);
    pthread_join(order_tid, NULL);

    return 0;
}
