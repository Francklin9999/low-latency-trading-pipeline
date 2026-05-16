#pragma once

#include <orderbook/types.hpp>
#include <orderbook/enums.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "hft/engine/universe.hpp"
#include "hft/engine/itch_book_adapter.hpp"
#include "hft/engine/sim/fill_sim.hpp"

namespace hft::strategy {

using SubmitFn = bool (*)(std::uint16_t locate, Side side, PriceTicks px,
                          Quantity qty, std::uint64_t parse_ns) noexcept;

struct Params {
    double      ema_alpha               = 0.05;
    PriceTicks  half_spread_ticks       = 1;
    PriceTicks  max_skew_ticks          = 3;
    double      inventory_aversion      = 0.01;
    Quantity    quote_qty               = 100;
    std::int32_t max_inventory          = 500;
    PriceTicks  requote_threshold_ticks = 1;

    // Quote-rate limits.
    std::int32_t per_symbol_msgs_per_sec = 100;
    std::int32_t aggregate_msgs_per_sec  = 1000;
    std::int32_t burst_msgs              = 50;

    // Inventory bands (multipliers on max_inventory).
    double soft_band_frac  = 0.7;
    double hard_band_frac  = 1.0;
    double panic_band_frac = 1.5;
    double soft_band_aversion_mult = 3.0;
};

struct alignas(64) State {
    double        ema_mid       = 0.0;
    double        last_mid      = 0.0;
    double        avg_entry_px  = 0.0;
    bool          ema_init      = false;
    std::atomic<bool> panic     = false;       // sticky; manual re-arm only
    std::int32_t  inventory     = 0;
    std::int64_t  realized_pnl_ticks = 0;
    PriceTicks    resting_bid   = 0;
    PriceTicks    resting_ask   = 0;
    std::uint64_t total_quotes  = 0;
    std::uint64_t total_fills   = 0;
    std::uint64_t rate_limited  = 0;           // requotes skipped by rate budget

    // Per-symbol token bucket; refilled from event.ts.
    std::int32_t  tokens        = 0;
    std::uint64_t last_refill_ts = 0;

    std::uint64_t skip_no_tob    = 0;   // book missing best-bid or best-ask
    std::uint64_t skip_crossed   = 0;   // bb >= ba (broken / auction)
    std::uint64_t skip_bad_px    = 0;   // computed bid/ask invalid
    std::uint64_t skip_hard_band = 0;   // one-sided by inventory-risk design
    std::uint64_t skip_panic     = 0;   // global kill or per-symbol panic
    std::uint64_t quoted_both    = 0;   // both sides resting after exit
};

class MarketMaker {
public:
    explicit MarketMaker(Params p = {}, SubmitFn submit = nullptr) noexcept
        : params_(p), submit_(submit)
    {
        for (auto& s : states_) s.tokens = params_.burst_msgs;
        agg_tokens_ = params_.burst_msgs;
    }

    void set_submit(SubmitFn fn) noexcept { submit_ = fn; }

    void set_params(const Params& p) noexcept {
        params_ = p;
        for (auto& s : states_) s.tokens = params_.burst_msgs;
        agg_tokens_ = params_.burst_msgs;
    }

    void on_book_event(std::size_t shard, ItchBookAdapter& adapter,
                       sim::FillSim& fsim, std::uint64_t ts) noexcept
    {
        State& s = states_[shard];

        if (global_kill_.load(std::memory_order_relaxed)
            || s.panic.load(std::memory_order_relaxed)) {
            ++s.skip_panic;
            flatten(s, fsim);
            return;
        }

        refill_buckets(s, ts);

        OrderBook& book = adapter.book();
        if (!book.hasBid() || !book.hasAsk()) { ++s.skip_no_tob; return; }
        const PriceTicks bb = book.bestBidPrice();
        const PriceTicks ba = book.bestAskPrice();
        if (bb >= ba) { ++s.skip_crossed; return; }
        const double mid = 0.5 * (bb + ba);
        s.last_mid = mid;

        if (!s.ema_init) { s.ema_mid = mid; s.ema_init = true; }
        else { s.ema_mid = params_.ema_alpha * mid
                         + (1.0 - params_.ema_alpha) * s.ema_mid; }

        const std::int32_t inv      = s.inventory;
        const std::int32_t abs_inv  = std::abs(inv);
        const auto         max_inv  = params_.max_inventory;
        const bool soft_band  = abs_inv > static_cast<std::int32_t>(params_.soft_band_frac  * max_inv);
        const bool hard_band  = abs_inv > static_cast<std::int32_t>(params_.hard_band_frac  * max_inv);
        const bool panic_band = abs_inv > static_cast<std::int32_t>(params_.panic_band_frac * max_inv);

        if (panic_band) {
            s.panic.store(true, std::memory_order_relaxed);
            ++s.skip_panic;
            flatten(s, fsim);
            return;
        }

        const double aversion = soft_band
            ? params_.inventory_aversion * params_.soft_band_aversion_mult
            : params_.inventory_aversion;

        const double fv       = s.ema_mid;
        const double cap      = static_cast<double>(params_.max_skew_ticks);
        const double rev_skew = std::clamp(mid - fv, -cap, cap);
        const double inv_skew = -static_cast<double>(inv) * aversion;

        const double bid_t = fv - params_.half_spread_ticks - rev_skew + inv_skew;
        const double ask_t = fv + params_.half_spread_ticks - rev_skew + inv_skew;
        const auto   bid_px = static_cast<PriceTicks>(std::floor(bid_t));
        const auto   ask_px = static_cast<PriceTicks>(std::ceil (ask_t));
        if (bid_px <= 0 || bid_px >= ask_px) { ++s.skip_bad_px; return; }

        const PriceTicks thr = params_.requote_threshold_ticks;
        const bool requote_b = (s.resting_bid == 0)
                            || std::abs(bid_px - s.resting_bid) >= thr;
        const bool requote_a = (s.resting_ask == 0)
                            || std::abs(ask_px - s.resting_ask) >= thr;

        const bool block_buy  = hard_band && (inv > 0);
        const bool block_sell = hard_band && (inv < 0);
        if (block_buy || block_sell) ++s.skip_hard_band;

        const std::uint16_t locate = universe::kLocates[shard];

        if (block_buy && s.resting_bid != 0) {
            if (try_consume_msg(s)) {
                fsim.onCancel(Side::Buy);
                emit_cancel(locate, Side::Buy, s.resting_bid, ts);
                s.resting_bid = 0;
            }
        }
        if (block_sell && s.resting_ask != 0) {
            if (try_consume_msg(s)) {
                fsim.onCancel(Side::Sell);
                emit_cancel(locate, Side::Sell, s.resting_ask, ts);
                s.resting_ask = 0;
            }
        }

        if (requote_b && !block_buy) {
            if (try_consume_msg(s)) {
                const Quantity qa = (bid_px == bb) ? book.bestBidQty() : 0;
                fsim.onQuote(Side::Buy, bid_px, params_.quote_qty, qa);
                emit_quote(locate, Side::Buy, bid_px, params_.quote_qty, ts);
                s.resting_bid = bid_px;
                ++s.total_quotes;
            } else { ++s.rate_limited; }
        }
        if (requote_a && !block_sell) {
            if (try_consume_msg(s)) {
                const Quantity qa = (ask_px == ba) ? book.bestAskQty() : 0;
                fsim.onQuote(Side::Sell, ask_px, params_.quote_qty, qa);
                emit_quote(locate, Side::Sell, ask_px, params_.quote_qty, ts);
                s.resting_ask = ask_px;
                ++s.total_quotes;
            } else { ++s.rate_limited; }
        }

        if (s.resting_bid != 0 && s.resting_ask != 0) ++s.quoted_both;
    }

    void on_fill(std::size_t shard, const sim::Fill& f) noexcept
    {
        State& s = states_[shard];
        apply_fill_pnl(s, f);
        if (f.side == Side::Buy) s.resting_bid = 0;
        else                     s.resting_ask = 0;
        ++s.total_fills;
    }

    const State&  state(std::size_t shard) const noexcept { return states_[shard]; }
    const Params& params() const noexcept { return params_; }
    std::int64_t unrealized_pnl_ticks(std::size_t shard) const noexcept
    {
        const State& s = states_[shard];
        return static_cast<std::int64_t>(
            std::llround(static_cast<double>(s.inventory)
                         * (s.last_mid - s.avg_entry_px)));
    }

    static std::atomic<bool>& global_kill_switch() noexcept { return global_kill_; }
    void arm(std::size_t shard) noexcept
    {
        states_[shard].panic.store(false, std::memory_order_relaxed);
    }
    void kill(std::size_t shard) noexcept
    {
        states_[shard].panic.store(true, std::memory_order_relaxed);
    }
    void kill_global() noexcept
    {
        global_kill_.store(true, std::memory_order_release);
    }
    void arm_all() noexcept
    {
        global_kill_.store(false, std::memory_order_release);
        for (auto& s : states_) s.panic.store(false, std::memory_order_relaxed);
    }

private:
    void refill_buckets(State& s, std::uint64_t ts) noexcept
    {
        // Per-symbol.
        if (s.last_refill_ts == 0) { s.last_refill_ts = ts; }
        else if (ts > s.last_refill_ts) {
            const std::uint64_t dt = ts - s.last_refill_ts;
            const std::int32_t add = static_cast<std::int32_t>(
                (dt * params_.per_symbol_msgs_per_sec) / 1'000'000'000ULL);
            if (add > 0) {
                s.last_refill_ts = ts;
                s.tokens = std::min(s.tokens + add, params_.burst_msgs);
            }
        }
        // Aggregate.
        if (last_agg_refill_ == 0) { last_agg_refill_ = ts; }
        else if (ts > last_agg_refill_) {
            const std::uint64_t dt = ts - last_agg_refill_;
            const std::int32_t add = static_cast<std::int32_t>(
                (dt * params_.aggregate_msgs_per_sec) / 1'000'000'000ULL);
            if (add > 0) {
                last_agg_refill_ = ts;
                agg_tokens_ = std::min(agg_tokens_ + add, params_.burst_msgs);
            }
        }
    }

    bool try_consume_msg(State& s) noexcept
    {
        if (s.tokens <= 0 || agg_tokens_ <= 0) return false;
        --s.tokens;
        --agg_tokens_;
        return true;
    }

    void flatten(State& s, sim::FillSim& fsim) noexcept
    {
        // No emit_cancel here: panic/kill flatten is local; we tear down sim
        // state but the wire-side mass-cancel is sent once by the kill path.
        if (s.resting_bid != 0) { fsim.onCancel(Side::Buy);  s.resting_bid = 0; }
        if (s.resting_ask != 0) { fsim.onCancel(Side::Sell); s.resting_ask = 0; }
    }

    void emit_quote(std::uint16_t locate, Side side, PriceTicks px,
                    Quantity qty, std::uint64_t ts) noexcept
    {
        if (submit_) (void)submit_(locate, side, px, qty, ts);
    }

    void emit_cancel(std::uint16_t locate, Side side, PriceTicks px,
                     std::uint64_t ts) noexcept
    {
        // qty=0 is the cancel discriminator on the wire.
        if (submit_) (void)submit_(locate, side, px, 0, ts);
    }

    void apply_fill_pnl(State& s, const sim::Fill& f) noexcept
    {
        const std::int32_t qty = static_cast<std::int32_t>(f.qty);
        const std::int32_t signed_qty = (f.side == Side::Buy) ? qty : -qty;
        const std::int32_t old_inv = s.inventory;
        const std::int32_t new_inv = old_inv + signed_qty;
        const double px = static_cast<double>(f.px);

        if (old_inv == 0 || (old_inv > 0 && signed_qty > 0)
            || (old_inv < 0 && signed_qty < 0)) {
            const double old_abs = static_cast<double>(std::abs(old_inv));
            const double new_abs = static_cast<double>(std::abs(new_inv));
            s.avg_entry_px = (old_abs == 0.0)
                ? px
                : ((old_abs * s.avg_entry_px)
                   + (static_cast<double>(qty) * px)) / new_abs;
            s.inventory = new_inv;
            return;
        }

        const std::int32_t closing = std::min(std::abs(old_inv), qty);
        if (old_inv > 0) {
            s.realized_pnl_ticks += static_cast<std::int64_t>(
                std::llround((px - s.avg_entry_px) * closing));
        } else {
            s.realized_pnl_ticks += static_cast<std::int64_t>(
                std::llround((s.avg_entry_px - px) * closing));
        }

        s.inventory = new_inv;
        if (new_inv == 0) {
            s.avg_entry_px = 0.0;
        } else if ((old_inv > 0 && new_inv < 0) || (old_inv < 0 && new_inv > 0)) {
            s.avg_entry_px = px;
        }
    }

    Params                              params_;
    SubmitFn                            submit_          = nullptr;
    std::array<State, universe::kSize>  states_{};
    std::int32_t                        agg_tokens_      = 0;
    std::uint64_t                       last_agg_refill_ = 0;
    inline static std::atomic<bool>     global_kill_{false};
};

}  // namespace hft::strategy
