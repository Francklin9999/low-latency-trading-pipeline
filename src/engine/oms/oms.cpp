#include "hft/engine/oms/oms.hpp"
#include "hft/ring_buffers/order/order_to_exc.h"
#include <atomic>
extern "C" {
    #include "hft/utils/utils.h"
}

namespace oms {

// Single-producer hot path: no CAS, no atomic fetch_add, no clock_gettime.
static uint64_t next_order_id = 1;

uint64_t submit(uint16_t stock_locate, Side side,
                uint32_t price, uint32_t qty, uint64_t parse_ns) noexcept {
    std::atomic_ref<uint32_t> nw{ORDER_TO_EXC->next_write};
    std::atomic_ref<uint32_t> nr{ORDER_TO_EXC->next_read};

    const uint32_t w = nw.load(std::memory_order_relaxed);
    const uint32_t r = nr.load(std::memory_order_acquire);
    if (w - r >= ORDER_TO_EXC_SIZE) return 0;

    const uint64_t id = next_order_id++;

    order& slot = ORDER_TO_EXC->data[w & ORDER_TO_EXC_MASK];
    slot.order_id     = id;
    slot.price        = price;
    slot.qty          = qty;
    slot.stock_locate = stock_locate;
    slot.side         = static_cast<uint8_t>(side);
    slot.parse_ns     = parse_ns;
    // slot.ts intentionally unwritten; order_sender stamps send_ns instead.

    nw.store(w + 1, std::memory_order_release);
    return id;
}

}  // namespace oms
