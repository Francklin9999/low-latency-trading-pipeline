#include "hft/engine/oms/oms.hpp"
#include <atomic>
#include <chrono>
extern "C" {
    #include "hft/utils/utils.h"
}

namespace oms {

static std::atomic<uint64_t> next_order_id{1};
static std::atomic<uint32_t> claimed_ptr{0};

uint64_t submit(uint16_t stock_locate, Side side,
                uint32_t price, uint32_t qty, uint64_t parse_ns) noexcept {
    std::atomic<uint32_t>& nw = ORDER_TO_EXC->next_write;
    std::atomic<uint32_t>& nr = ORDER_TO_EXC->next_read;

    uint32_t w = claimed_ptr.load(std::memory_order_relaxed);
    while (true) {
        const uint32_t r = nr.load(std::memory_order_acquire);
        if (w - r >= ORDER_TO_EXC_SIZE) return 0;
        if (claimed_ptr.compare_exchange_weak(w, w + 1,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed))
            break;
        cpu_relax();
    }

    const uint64_t ts = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const uint64_t id = next_order_id.fetch_add(1, std::memory_order_relaxed);

    order& slot = ORDER_TO_EXC->data[w & ORDER_TO_EXC_MASK];
    slot.order_id = id;
    slot.ts = ts;
    slot.price = price;
    slot.qty = qty;
    slot.stock_locate = stock_locate;
    slot.side = static_cast<uint8_t>(side);
    slot.parse_ns = parse_ns;

    uint32_t expected = w;
    while (!nw.compare_exchange_weak(expected, w + 1,
                                     std::memory_order_release,
                                     std::memory_order_relaxed)) {
        expected = w;
        cpu_relax();
    }

    return id;
}

uint32_t drain(order* buf, uint32_t max_orders) noexcept {
    std::atomic<uint32_t>& nw = ORDER_TO_EXC->next_write;
    std::atomic<uint32_t>& nr = ORDER_TO_EXC->next_read;

    const uint32_t r = nr.load(std::memory_order_relaxed);
    const uint32_t w = nw.load(std::memory_order_acquire);
    const uint32_t avail = w - r;
    const uint32_t count = avail < max_orders ? avail : max_orders;

    for (uint32_t i = 0; i < count; ++i)
        buf[i] = ORDER_TO_EXC->data[(r + i) & ORDER_TO_EXC_MASK];

    nr.store(r + count, std::memory_order_release);
    return count;
}

}
