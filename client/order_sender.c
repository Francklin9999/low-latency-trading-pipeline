#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <strings.h>
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
#include "client.h"
#include "../utils/utils.h"
#include "config.h"

#define RECONNECT_DELAY_S 1
#define SHM_OPEN_RETRY_S  1

static uint64_t sender_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int connect_to_exchange(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("[order_sender] socket"); return -1; }

    /* Disable Nagle — every order must be flushed immediately, not batched */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ORDER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);

    while (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[order_sender] connect, retrying...");
        sleep(RECONNECT_DELAY_S);
    }

    /* Non-blocking: send() returns EAGAIN instead of sleeping this thread */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    printf("[order_sender] connected to %s:%d\n", SERVER_IP, ORDER_PORT);
    return fd;
}

void *order_sender_thread(void *arg) {
    (void)arg;

    int fd_order;
    while ((fd_order = shm_open(SHM_ORDER_TO_EXC, O_RDWR, 0)) == -1) {
        fprintf(stderr, "[order_sender] shm_open(%s) failed: %s — retrying in %ds\n",
                SHM_ORDER_TO_EXC, strerror(errno), SHM_OPEN_RETRY_S);
        sleep(SHM_OPEN_RETRY_S);
    }
    ORDER_TO_EXC = (order_to_exc*) mmap(
        NULL, sizeof(order_to_exc), PROT_READ | PROT_WRITE,
        MAP_SHARED, fd_order, 0);
    if (ORDER_TO_EXC == MAP_FAILED) {
        fprintf(stderr, "[order_sender] mmap failed: %s\n", strerror(errno));
        close(fd_order);
        return NULL;
    }
    close(fd_order);

    int fd = connect_to_exchange();
    if (fd < 0) return NULL;

    while (1) {
        uint32_t r = atomic_load_explicit(&ORDER_TO_EXC->next_read,  memory_order_relaxed);
        uint32_t w = atomic_load_explicit(&ORDER_TO_EXC->next_write, memory_order_acquire);

        if (r == w) {
            cpu_relax();
            continue;
        }

        order* slot = &ORDER_TO_EXC->data[r & ORDER_TO_EXC_MASK];
        slot->send_ns = sender_now_ns();

        const char* buf       = (const char*)slot;
        size_t      remaining = sizeof(order);

        while (remaining > 0) {
            ssize_t n = send(fd, buf, remaining, MSG_NOSIGNAL);
            if (n > 0) {
                buf       += n;
                remaining -= (size_t)n;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Kernel send buffer momentarily full — busy-spin, stay on CPU */
                cpu_relax();
            } else {
                perror("[order_sender] send, reconnecting...");
                close(fd);
                fd = connect_to_exchange();
                if (fd < 0) goto done;
                buf       = (const char*)&ORDER_TO_EXC->data[r & ORDER_TO_EXC_MASK];
                remaining = sizeof(order);
            }
        }

        atomic_store_explicit(&ORDER_TO_EXC->next_read, r + 1, memory_order_release);
    }

    done:
        close(fd);
        return NULL;
}
