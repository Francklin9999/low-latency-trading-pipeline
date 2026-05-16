#pragma once

#include <cstdint>
#include "hft/engine/oms/order.hpp"
#include "hft/engine/strategy/signal.hpp"

static constexpr uint32_t RISK_MAX_POSITION_PER_STOCK = 10000;
static constexpr uint32_t RISK_MAX_ORDER_QTY          = 500;
static constexpr uint64_t RISK_MAX_ORDER_NOTIONAL     = UINT64_MAX;
static constexpr uint64_t RISK_MAX_DAILY_NOTIONAL     = UINT64_MAX;

// Defaults -- overridable at runtime via risk::set_limits() from env vars.
static constexpr uint32_t RISK_MAX_ORDERS_PER_SEC = 100;
static constexpr uint32_t RISK_ORDER_BURST        = 50;

namespace risk {

bool check_and_fill(const Signal& sig) noexcept;

// Reconfigure the rate-limit knobs at startup. Resets the token bucket to
// the new burst capacity. Pass 0 to either to keep the current value.
void set_limits(uint32_t orders_per_sec, uint32_t burst) noexcept;

}  // namespace risk
