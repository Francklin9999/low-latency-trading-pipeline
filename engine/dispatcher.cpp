#define _POSIX_C_SOURCE 200809L
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

#include "batch_sizes.hpp"
#include "batch_structs.hpp"
#include "dispatcher.hpp"
#include "./cpu/cpu_entry.hpp"
#include "./strategy/imbalance_strat.hpp"
#include "./risk/risk.hpp"
#include "./oms/oms.hpp"

extern "C" {
    #include "../utils/utils.h"
    #include "events.h"
    #include "../config.h"
}
// Ring buffer headers must be outside extern "C": they include <atomic> when
// compiled as C++, and C++ standard library headers cannot appear inside an
// extern "C" block 
#include "../ring_buffers/event/event_to_engine.h"
#include "../ring_buffers/order/order_to_exc.h"

#ifndef HFT_ENGINE_DISPATCH_CORE
#define HFT_ENGINE_DISPATCH_CORE 6
#endif
#define MAX_SIGNALS_PER_BATCH 256
#define MAX_SIGNALS_PER_LANE (MAX_SIGNALS_PER_BATCH / NUMBER_OF_DISPATCHERS)

BatchBuffer g_buf[NUMBER_OF_PING_PONG_BUFFERS] {};
int g_ping = 0;

Signal signal_buf[NUMBER_OF_DISPATCHERS][MAX_SIGNALS_PER_LANE];

event_to_engine* EVENT_ENGINE = NULL;
order_to_exc* ORDER_TO_EXC = NULL;

#ifdef __linux__
static void pin_self_to_core(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        fprintf(stderr, "[engine] pin to core %d failed\n", core);
}
#endif

inline void append_add(const event& in) {
    const int lane = in.order_id % NUMBER_OF_DISPATCHERS;
    AddOrderBatch& batch = g_buf[g_ping].addOrder[lane];
    const std::uint32_t idx = batch.n++;
    batch.oid[idx] = in.order_id;
    batch.ts[idx] = in.ts;
    batch.price[idx] = in.price;
    batch.qty[idx] = in.qty;
    batch.loc[idx] = in.stock_locate;
    batch.side[idx] = in.side;
    batch.notional[idx] = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(in.price) * in.qty);
}

inline void append_add_mpid(const event& in) {
    const int lane = in.order_id % NUMBER_OF_DISPATCHERS;
    AddOrderMpidBatch& batch = g_buf[g_ping].addOrderMpid[lane];
    const std::uint32_t idx = batch.n++;
    batch.oid[idx] = in.order_id;
    batch.ts[idx] = in.ts;
    batch.price[idx] = in.price;
    batch.qty[idx] = in.qty;
    batch.loc[idx] = in.stock_locate;
    batch.side[idx] = in.side;
    batch.notional[idx] = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(in.price) * in.qty);
}

inline void append_modify(const event& in) {
    const int lane = in.order_id % NUMBER_OF_DISPATCHERS;
    OrderModifyBatch& batch = g_buf[g_ping].orderModify[lane];
    const std::uint32_t idx = batch.n++;
    batch.oid[idx] = in.order_id;
    batch.ts[idx] = in.ts;
    batch.qty[idx] = in.qty;
    batch.price[idx] = in.price;
    batch.loc[idx] = in.stock_locate;
}

inline void append_exec(const event& in) {
    const int lane = in.order_id % NUMBER_OF_DISPATCHERS;
    OrderExecutedPriceBatch& batch = g_buf[g_ping].orderExecPrice[lane];
    const std::uint32_t idx = batch.n++;
    batch.oid[idx] = in.order_id;
    batch.ts[idx] = in.ts;
    batch.qty[idx] = in.qty;
    batch.price[idx] = in.price;
    batch.loc[idx] = in.stock_locate;
}

inline void append_cancel(const event& in) {
    const int lane = in.order_id % NUMBER_OF_DISPATCHERS;
    OrderCancelBatch& batch = g_buf[g_ping].orderCancel[lane];
    const std::uint32_t idx = batch.n++;
    batch.oid[idx] = in.order_id;
    batch.ts[idx] = in.ts;
    batch.qty[idx] = in.qty;
    batch.loc[idx] = in.stock_locate;
}

inline void append_delete(const event& in) {
    const int lane = in.order_id % NUMBER_OF_DISPATCHERS;
    OrderDeleteBatch& batch = g_buf[g_ping].orderDelete[lane];
    const std::uint32_t idx = batch.n++;
    batch.oid[idx] = in.order_id;
    batch.ts[idx] = in.ts;
    batch.loc[idx] = in.stock_locate;
}

inline void append_replace(const event& in) {
    const int lane = in.order_id % NUMBER_OF_DISPATCHERS;
    OrderReplaceBatch& batch = g_buf[g_ping].orderReplace[lane];
    const std::uint32_t idx = batch.n++;
    batch.new_oid[idx] = in.order_id;
    batch.ts[idx] = in.ts;
    batch.price[idx] = in.price;
    batch.qty[idx] = in.qty;
    batch.loc[idx] = in.stock_locate;
    batch.side[idx] = in.side;
}

void process_lane(BatchBuffer& batch, int lane) {
    cpu_op::run_lane(batch, lane);
    const int n = imbalance::evaluate(batch.addOrder[lane], batch.addOrderMpid[lane],
                                      signal_buf[lane], MAX_SIGNALS_PER_LANE, lane);
    for (int i = 0; i < n; ++i) {
        const Signal& sig = signal_buf[lane][i];
        if (!risk::check_and_fill(sig)) continue;
        oms::submit(sig.stock_locate, sig.side, sig.price, sig.qty, sig.parse_ns);
    }
}

void dispatch_to_compute(BatchBuffer& batch) {
    process_lane(batch, 0);
    process_lane(batch, 1);
}

void reset_lane(BatchBuffer& batch, int lane) {
    batch.addOrder[lane].n = 0;
    batch.addOrderMpid[lane].n = 0;
    batch.orderModify[lane].n = 0;
    batch.orderExecPrice[lane].n = 0;
    batch.orderCancel[lane].n = 0;
    batch.orderDelete[lane].n = 0;
    batch.orderReplace[lane].n = 0;
}

void reset_all_batches(BatchBuffer& batch) {
    reset_lane(batch, 0);
    reset_lane(batch, 1);
}

void consume() {
    for (;;) {
        const int cur_ping = g_ping;
        const int cur_pong = 1 - cur_ping;

        const std::uint32_t tail = EVENT_ENGINE->next_read.load(std::memory_order_relaxed);
        const std::uint32_t head = EVENT_ENGINE->next_write.load(std::memory_order_acquire);

        const std::uint32_t avail   = (std::uint32_t)(head - tail);
        const std::uint32_t to_read = (avail > BATCH_MAX) ? BATCH_MAX : avail;

        if (to_read == 0) { cpu_relax(); continue; }

        for (std::uint32_t i = 0; i < to_read; ++i) {
            const event e = EVENT_ENGINE->data[(tail + i) & EVENT_ENGINE_MASK];
            switch (e.type) {
                case 'A': append_add(e); break;
                case 'F': append_add_mpid(e); break;
                case 'E': append_modify(e); break;
                case 'C': append_exec(e); break;
                case 'X': append_cancel(e); break;
                case 'D': append_delete(e); break;
                case 'U': append_replace(e); break;
                default: break;
            }
        }

        EVENT_ENGINE->next_read.store(tail + to_read, std::memory_order_release);

        g_ping = cur_pong;
        dispatch_to_compute(g_buf[cur_ping]);
        reset_all_batches(g_buf[cur_pong]);
    }
}

int main() {
#ifdef __linux__
    pin_self_to_core(HFT_ENGINE_DISPATCH_CORE);
    printf("[engine] dispatcher/compute/reset: core %d\n", HFT_ENGINE_DISPATCH_CORE);
#endif

    if (shm_unlink(SHM_EVENT_TO_STRAT) == -1) {
        if (errno != ENOENT) {
            fprintf(stderr, "shm_unlink(%s) failed: %s\n",
                    SHM_EVENT_TO_STRAT, strerror(errno));
            fflush(stderr);
        }
    }
    int fd_event = shm_open(SHM_EVENT_TO_STRAT, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (fd_event == -1) { perror("fd event error"); _exit(1); }
    if (ftruncate(fd_event, sizeof(event_to_engine)) == -1) {
        perror("ftruncate fd event");
        _exit(1);
    }

    EVENT_ENGINE = (event_to_engine*) mmap(NULL, sizeof(event_to_engine), PROT_READ | PROT_WRITE, MAP_SHARED, fd_event, 0);
    if (EVENT_ENGINE == MAP_FAILED) { perror("map failed event"); _exit(1); }

    EVENT_ENGINE->next_write.store(0, std::memory_order_relaxed);
    EVENT_ENGINE->next_read.store(0, std::memory_order_relaxed);

    close(fd_event);

    if (shm_unlink(SHM_ORDER_TO_EXC) == -1) {
        if (errno != ENOENT) {
            fprintf(stderr, "shm_unlink(%s) failed: %s\n",
                    SHM_ORDER_TO_EXC, strerror(errno));
            fflush(stderr);
        }
    }
    int fd_order = shm_open(SHM_ORDER_TO_EXC, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (fd_order == -1) { perror("fd event error"); _exit(1); }
    if (ftruncate(fd_order, sizeof(order_to_exc)) == -1) {
        perror("ftruncate fd event");
        _exit(1);
    }

    ORDER_TO_EXC = (order_to_exc*) mmap(NULL, sizeof(order_to_exc), PROT_READ | PROT_WRITE, MAP_SHARED, fd_order, 0);
    if (ORDER_TO_EXC == MAP_FAILED) { perror("map failed event"); _exit(1); }

    consume();

    return 0;
}
