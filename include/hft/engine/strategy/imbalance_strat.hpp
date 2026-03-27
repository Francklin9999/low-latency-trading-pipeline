#pragma once

#include <cstdint>
#include "hft/engine/batch_structs.hpp"
#include "hft/engine/oms/order.hpp"

static constexpr float IMBAL_BUY_THRESHOLD  = 0.20f;
static constexpr float IMBAL_SELL_THRESHOLD = 0.20f;
static constexpr uint64_t IMBAL_MIN_TOTAL_QTY = 1;
static constexpr uint32_t IMBAL_ORDER_QTY = 1;

struct Signal {
    uint16_t stock_locate;
    uint32_t price;
    uint32_t qty;
    Side side;
    uint64_t parse_ns;
};

namespace imbalance {

int evaluate(const AddOrderBatch&     addOrder,
             const AddOrderMpidBatch& addOrderMpid,
             Signal* out, int max_out, int lane) noexcept;

}
