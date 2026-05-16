#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

#include <orderbook/types.hpp>
#include <orderbook/enums.hpp>

namespace hft::sim {

struct RestingQuote {
    PriceTicks px          = 0;
    Quantity   qty         = 0;
    Quantity   queue_ahead = 0;
};

struct Fill {
    Side          side;
    PriceTicks    px;
    Quantity      qty;
    std::uint64_t ts;
};

class FillSim {
public:
    void onQuote(Side side, PriceTicks px, Quantity qty,
                 Quantity queue_ahead) noexcept
    {
        quote(side) = RestingQuote{px, qty, queue_ahead};
    }

    void onCancel(Side side) noexcept
    {
        quote(side) = RestingQuote{};
    }

    std::optional<Fill> onTrade(Side resting_side, PriceTicks trade_px,
                                Quantity trade_qty,
                                std::uint64_t ts) noexcept
    {
        RestingQuote& q = quote(resting_side);
        if (q.qty <= 0 || trade_qty <= 0) return std::nullopt;

        const bool our_px_better = (resting_side == Side::Buy)
                                       ? (q.px > trade_px)
                                       : (q.px < trade_px);
        const bool our_px_equal = (q.px == trade_px);
        if (!our_px_better && !our_px_equal) return std::nullopt;

        const PriceTicks fill_px = q.px;
        Quantity         fill_qty = 0;

        if (our_px_better) {
            // Sweep through our level: full fill at our price.
            fill_qty      = q.qty;
            q.qty         = 0;
            q.queue_ahead = 0;
        } else {
            // At-level: queue eats first, residual fills us.
            const Quantity eat      = std::min(q.queue_ahead, trade_qty);
            q.queue_ahead          -= eat;
            const Quantity residual = trade_qty - eat;
            fill_qty                = std::min(q.qty, residual);
            q.qty                  -= fill_qty;
            if (q.qty == 0) q.queue_ahead = 0;
        }

        if (fill_qty == 0) return std::nullopt;
        return Fill{resting_side, fill_px, fill_qty, ts};
    }

    const RestingQuote& bid() const noexcept { return bid_; }
    const RestingQuote& ask() const noexcept { return ask_; }

private:
    RestingQuote& quote(Side s) noexcept
    {
        return (s == Side::Buy) ? bid_ : ask_;
    }

    RestingQuote bid_{};
    RestingQuote ask_{};
};

}  // namespace hft::sim
