#include "hft/engine/risk/risk.hpp"

#include <cstdint>

namespace risk {

// Hot path is single-threaded; no atomics needed. Refill is inline using
// Signal.parse_ns (the off-path refill thread starved under SCHED_FIFO).
static int64_t  rl_tokens       = static_cast<int64_t>(RISK_ORDER_BURST);
static uint64_t rl_last_ns      = UINT64_MAX;   // sentinel: unprimed
static int32_t  positions[65536]{};
static uint64_t daily_notional  = 0;

// Runtime-tunable copies of the rate-limit knobs. Initialised to the
// compile-time defaults; set_limits() updates them at startup.
static uint32_t rl_orders_per_sec = RISK_MAX_ORDERS_PER_SEC;
static uint32_t rl_burst          = RISK_ORDER_BURST;

void set_limits(uint32_t orders_per_sec, uint32_t burst) noexcept
{
    if (orders_per_sec > 0) rl_orders_per_sec = orders_per_sec;
    if (burst > 0)          { rl_burst = burst; rl_tokens = static_cast<int64_t>(burst); }
}

static inline void inline_refill(uint64_t now_ns) noexcept
{
    if (rl_last_ns == UINT64_MAX) { rl_last_ns = now_ns; return; }
    if (now_ns <= rl_last_ns) return;
    const uint64_t elapsed = now_ns - rl_last_ns;
    const int64_t  add     = static_cast<int64_t>(
        (elapsed * rl_orders_per_sec) / 1'000'000'000ULL);
    if (add <= 0) return;
    rl_last_ns = now_ns;
    const int64_t want = rl_tokens + add;
    rl_tokens = (want > static_cast<int64_t>(rl_burst))
              ? static_cast<int64_t>(rl_burst) : want;
}

bool check_and_fill(const Signal& s) noexcept
{
    inline_refill(s.parse_ns);

    const uint64_t notional = static_cast<uint64_t>(s.price) * s.qty;
    const int32_t  delta    = (s.side == oms::Side::BUY)
                            ?  static_cast<int32_t>(s.qty)
                            : -static_cast<int32_t>(s.qty);
    const int32_t  new_pos  = positions[s.stock_locate] + delta;

    const bool ok =
          (rl_tokens                    >  0)
        & (s.qty                        <= RISK_MAX_ORDER_QTY)
        & (notional                     <= RISK_MAX_ORDER_NOTIONAL)
        & (daily_notional + notional    <= RISK_MAX_DAILY_NOTIONAL)
        & (new_pos                      <=  static_cast<int32_t>(RISK_MAX_POSITION_PER_STOCK))
        & (new_pos                      >= -static_cast<int32_t>(RISK_MAX_POSITION_PER_STOCK));

    if (!ok) return false;
    --rl_tokens;
    positions[s.stock_locate] = new_pos;
    daily_notional           += notional;
    return true;
}

}  // namespace risk
