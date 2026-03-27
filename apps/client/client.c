#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include "hft/client/client.h"

#ifdef __linux__
#include <sched.h>
#endif

#ifndef HFT_CLIENT_UDP_RECV_CORE
#define HFT_CLIENT_UDP_RECV_CORE 4
#endif
#ifndef HFT_CLIENT_ORDER_SEND_CORE
#define HFT_CLIENT_ORDER_SEND_CORE 5
#endif

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

int main(void) {
    pthread_t recv_tid, send_tid;
    pthread_attr_t recv_attr, send_attr;
    pthread_attr_init(&recv_attr);
    pthread_attr_init(&send_attr);

#ifdef __linux__
    if (pin_attr_to_core(&recv_attr, HFT_CLIENT_UDP_RECV_CORE)   != 0) return 1;
    if (pin_attr_to_core(&send_attr, HFT_CLIENT_ORDER_SEND_CORE) != 0) return 1;
    printf("[client] udp_receiver  → core %d\n", HFT_CLIENT_UDP_RECV_CORE);
    printf("[client] order_sender  → core %d\n", HFT_CLIENT_ORDER_SEND_CORE);
#endif

    if (pthread_create(&recv_tid, &recv_attr, udp_receiver_thread, NULL) != 0) {
        perror("pthread_create udp_receiver");
        return 1;
    }

    if (pthread_create(&send_tid, &send_attr, order_sender_thread, NULL) != 0) {
        perror("pthread_create order_sender");
        return 1;
    }

    pthread_attr_destroy(&recv_attr);
    pthread_attr_destroy(&send_attr);

    pthread_join(recv_tid, NULL);
    pthread_join(send_tid, NULL);

    return 0;
}
