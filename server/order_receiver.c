#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <inttypes.h>

#include "server.h"
#include "../engine/oms/order.h"

#define BACKLOG      16
#define BUF_SIZE     4096
#define RESULTS_DIR  "results"

#define LOG_BUF_SIZE (4 * 1024 * 1024)
#define LOG_FLUSH_AT (LOG_BUF_SIZE - 1024)

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void flush_log_buffer(char *buf, size_t *len, FILE *f)
{
    if (*len == 0)
        return;

    fwrite(buf, 1, *len, f);
    fflush(f);
    *len = 0;
}

static void log_order(char *log_buf, size_t *log_len, FILE *f,
                      const order *o, uint64_t recv_ns)
{
    if (*log_len >= LOG_FLUSH_AT)
        flush_log_buffer(log_buf, log_len, f);

    int64_t engine_ns = (int64_t)o->ts     - (int64_t)o->parse_ns;
    int64_t queue_ns  = (int64_t)o->send_ns - (int64_t)o->ts;
    int64_t wire_ns   = (int64_t)recv_ns   - (int64_t)o->send_ns;
    int64_t total_ns  = (int64_t)recv_ns   - (int64_t)o->parse_ns;

    const char *side = (o->side == 'B') ? "BUY" : "SELL";

    int written = snprintf(
        log_buf + *log_len,
        LOG_BUF_SIZE - *log_len,

        "[order #%" PRIu64 "] side=%-4s price=%u qty=%u locate=%u\n"
        "  engine (parse  -> enqueue): %lld ns / %.3f us / %.6f ms\n"
        "  queue  (enqueue -> send):   %lld ns / %.3f us / %.6f ms\n"
        "  wire   (send   -> recv):    %lld ns / %.3f us / %.6f ms\n"
        "  total  (parse  -> recv):    %lld ns / %.3f us / %.6f ms\n\n",

        (uint64_t)o->order_id,
        side,
        o->price,
        o->qty,
        (unsigned)o->stock_locate,

        (long long)engine_ns,
        engine_ns / 1000.0,
        engine_ns / 1000000.0,

        (long long)queue_ns,
        queue_ns / 1000.0,
        queue_ns / 1000000.0,

        (long long)wire_ns,
        wire_ns / 1000.0,
        wire_ns / 1000000.0,

        (long long)total_ns,
        total_ns / 1000.0,
        total_ns / 1000000.0);

    if (written > 0)
        *log_len += (size_t)written;
}

static void handle_client(int client_fd, struct sockaddr_in *client_addr)
{
    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr->sin_addr, addr_str, sizeof(addr_str));

    printf("[order_receiver] connection from %s:%d\n",
           addr_str, ntohs(client_addr->sin_port));

    int rc = mkdir(RESULTS_DIR, 0755);
    if (rc < 0 && errno != EEXIST)
        perror("[order_receiver] mkdir");

    char log_path[256];

    {
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);

        char ts_buf[32];
        strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", &tm_info);

        snprintf(log_path, sizeof(log_path),
                 RESULTS_DIR "/orders_%s_%d_%s.txt",
                 addr_str,
                 ntohs(client_addr->sin_port),
                 ts_buf);
    }

    FILE *log_f = fopen(log_path, "w");

    if (!log_f)
    {
        perror("[order_receiver] fopen");
        close(client_fd);
        return;
    }

    setvbuf(log_f, NULL, _IOFBF, 65536);

    printf("[order_receiver] logging to %s\n", log_path);

    char *log_buf = malloc(LOG_BUF_SIZE);

    if (!log_buf)
    {
        perror("[order_receiver] malloc");
        fclose(log_f);
        close(client_fd);
        return;
    }

    size_t log_len = 0;

    unsigned char buf[BUF_SIZE + sizeof(order)];
    size_t carry = 0;
    ssize_t n;

    uint64_t order_count = 0;
    uint64_t logged_count = 0;

    while ((n = recv(client_fd, buf + carry, BUF_SIZE, 0)) > 0)
    {
        size_t total = carry + (size_t)n;
        size_t offset = 0;

        while (offset + sizeof(order) <= total)
        {
            const order *o = (const order *)(buf + offset);

            uint64_t recv_ns = mono_ns();

            if (order_count % 100 == 0)
            {
                log_order(log_buf, &log_len, log_f, o, recv_ns);
                logged_count++;
            }

            order_count++;
            offset += sizeof(order);
        }

        carry = total - offset;

        if (carry > 0)
            memmove(buf, buf + offset, carry);
    }

    if (n < 0)
        perror("[order_receiver] recv");

    flush_log_buffer(log_buf, &log_len, log_f);

    free(log_buf);

    fclose(log_f);

    printf("[order_receiver] client %s:%d disconnected — %" PRIu64
           " orders received, %" PRIu64 " logged to %s\n",
           addr_str,
           ntohs(client_addr->sin_port),
           order_count,
           logged_count,
           log_path);

    close(client_fd);
}

void *order_receiver_thread(void *arg)
{
    (void)arg;

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0)
    {
        perror("[order_receiver] socket");
        return NULL;
    }

    int opt = 1;

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(ORDER_PORT);
    addr.sin_addr.s_addr = inet_addr(BIND_IP);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("[order_receiver] bind");
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, BACKLOG) < 0)
    {
        perror("[order_receiver] listen");
        close(server_fd);
        return NULL;
    }

    printf("[order_receiver] listening on %s:%d (TCP)\n", BIND_IP, ORDER_PORT);

    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr *)&client_addr,
                               &client_len);

        if (client_fd < 0)
        {
            perror("[order_receiver] accept");
            continue;
        }

        handle_client(client_fd, &client_addr);
    }

    close(server_fd);
    return NULL;
}
