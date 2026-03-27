#pragma once

#include <cstdint>
#include "hft/engine/oms/order.hpp"
#include "hft/engine/strategy/imbalance_strat.hpp"

// Risk limits
static constexpr uint32_t RISK_MAX_POSITION_PER_STOCK = 10000; // shares
static constexpr uint32_t RISK_MAX_ORDER_QTY = 500; // shares
static constexpr uint64_t RISK_MAX_ORDER_NOTIONAL = UINT64_MAX; // $
static constexpr uint64_t RISK_MAX_DAILY_NOTIONAL = UINT64_MAX; // $

// Order rate limit
static constexpr uint32_t RISK_MAX_ORDERS_PER_SEC = 100; // orders/s
static constexpr uint32_t RISK_ORDER_BURST = 50;   // max burst tokens

namespace risk {

bool check_and_fill(const Signal& sig) noexcept;

}
