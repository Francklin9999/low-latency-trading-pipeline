#include "risk.hpp"
#include <atomic>
#include <chrono>

namespace risk {

static std::atomic<int32_t>  positions[65536] {};
static std::atomic<uint64_t> daily_notional{0};

static std::atomic<uint64_t> rl_tokens{RISK_ORDER_BURST};
static std::atomic<uint64_t> rl_last_ns{0};

static inline uint64_t mono_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

static bool rate_check() noexcept {
    const uint64_t now  = mono_ns();
    uint64_t last = rl_last_ns.load(std::memory_order_relaxed);

    if (now > last) {
        const uint64_t elapsed_ns = now - last;
        const uint64_t add = (elapsed_ns * RISK_MAX_ORDERS_PER_SEC) / 1000000000ULL;
        if (add > 0 && rl_last_ns.compare_exchange_weak(
                last, now, std::memory_order_relaxed, std::memory_order_relaxed)) {
            uint64_t cur  = rl_tokens.load(std::memory_order_relaxed);
            uint64_t next = cur + add;
            if (next > RISK_ORDER_BURST) next = RISK_ORDER_BURST;
            rl_tokens.store(next, std::memory_order_relaxed);
        }
    }

    uint64_t cur = rl_tokens.load(std::memory_order_relaxed);
    do {
        if (cur == 0) return false;
    } while (!rl_tokens.compare_exchange_weak(
                 cur, cur - 1, std::memory_order_relaxed, std::memory_order_relaxed));

    return true;
}

bool check_and_fill(const Signal& signal) noexcept {
    if (!rate_check()) return false;
    if (signal.qty > RISK_MAX_ORDER_QTY) return false;

    const uint64_t notional = static_cast<uint64_t>(signal.price) * signal.qty;
    if (notional > RISK_MAX_ORDER_NOTIONAL) return false;

    uint64_t cur_dn = daily_notional.load(std::memory_order_relaxed);
    do {
        if (cur_dn + notional > RISK_MAX_DAILY_NOTIONAL) return false;
    } while (!daily_notional.compare_exchange_weak(
                 cur_dn, cur_dn + notional, std::memory_order_relaxed));

    const int32_t delta = (signal.side == Side::BUY)
                          ? +static_cast<int32_t>(signal.qty)
                          : -static_cast<int32_t>(signal.qty);

    int32_t cur_pos = positions[signal.stock_locate].load(std::memory_order_relaxed);
    do {
        const int32_t new_pos = cur_pos + delta;
        if (new_pos > static_cast<int32_t>(RISK_MAX_POSITION_PER_STOCK) ||
            new_pos < -static_cast<int32_t>(RISK_MAX_POSITION_PER_STOCK)) {
            daily_notional.fetch_sub(notional, std::memory_order_relaxed);
            return false;
        }
    } while (!positions[signal.stock_locate].compare_exchange_weak(
                 cur_pos, cur_pos + delta, std::memory_order_relaxed));

    return true;
}

}
