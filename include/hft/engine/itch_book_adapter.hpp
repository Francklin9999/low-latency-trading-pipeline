#pragma once

#include <orderbook/orderbook.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace hft {

class LiveOrderTable {
public:
    using OrderRef = std::uint64_t;

    explicit LiveOrderTable(std::size_t cap) {
        // round up to next power of two
        std::size_t n = 1;
        while (n < cap) n <<= 1;
        slots_.assign(n, Slot{0, nullptr});
        mask_ = n - 1;
    }
    std::int32_t find(OrderRef key) const noexcept {
        const std::size_t start = hash(key) & mask_;
        std::size_t i = start;
        // Bound the probe so a fully-tombstoned chain can't infinite-loop.
        const std::size_t limit = mask_ + 1u;
        for (std::size_t k = 0; k < limit; ++k) {
            const Slot& s = slots_[i];
            if (s.key == key && s.handle != nullptr) return (std::int32_t)i;
            if (s.key == kEmpty) return -1;
            i = (i + 1) & mask_;
        }
        return -1;
    }
    void insert(OrderRef key, OrderPointer h) noexcept {
        std::size_t i = hash(key) & mask_;
        for (;;) {
            Slot& s = slots_[i];
            if (s.key == kEmpty || s.key == kTombstone || s.key == key) {
                s.key = key; s.handle = h; return;
            }
            i = (i + 1) & mask_;
        }
    }
    void erase(std::int32_t idx) noexcept {
        // Tombstone (not empty): later probes need to skip past this slot.
        slots_[idx].key    = kTombstone;
        slots_[idx].handle = nullptr;
    }
    OrderPointer handle(std::int32_t idx) const noexcept {
        return slots_[idx].handle;
    }
private:
    struct Slot { OrderRef key; OrderPointer handle; };
    static constexpr OrderRef kEmpty     = 0;
    static constexpr OrderRef kTombstone = ~OrderRef{0} - 1;
    static std::size_t hash(OrderRef k) noexcept {
        // splitmix64 -- strong avalanche, two cycles per call
        k ^= k >> 30; k *= 0xbf58476d1ce4e5b9ULL;
        k ^= k >> 27; k *= 0x94d049bb133111ebULL;
        k ^= k >> 31;
        return (std::size_t)k;
    }
    std::vector<Slot> slots_;
    std::size_t       mask_{0};
};

class ItchBookAdapter {
public:
    using OrderRef = std::uint64_t;

    explicit ItchBookAdapter(std::int32_t poolSize = 1 << 20)
        : book_(poolSize), liveOrders_(static_cast<std::size_t>(poolSize) * 2u)
    {
        // 2x overallocation keeps the open-addressing table at <=50% load and
        // the average probe length at ~1.5 -- no rehash, no node allocations.
    }

    // ITCH 'A' (add order) and 'F' (add order with MPID attribution).
    // Identical book-level semantics; the MPID is irrelevant for reconstruction.
    void onAdd(OrderRef ref, char buySell,
               std::uint32_t shares, std::uint32_t itchPx) noexcept
    {
        const PriceTicks px = toTicks(itchPx);
        // OrderBook levels are array-indexed by tick price; ITCH stub quotes
        // at $146,402-class prices would index past MAX_LEVEL and segfault.
        if (!inBookRange(px)) return;
        AddCommand add{
            LimitOrder{ px, TimeInForce{} },
            static_cast<OrderId>(ref),                  // truncated; orderbook uses orderId only for stops
            static_cast<Quantity>(shares),
            sideFromChar(buySell),
        };
        Command cmd = add;
        if (OrderPointer handle = book_.Process(cmd)) {
            liveOrders_.insert(ref, handle);
        }
        // Null handle => the add fully matched on insertion (rare); no map entry needed.
    }

    // ITCH 'X' -- cancel of `cancelledShares` from the resting residual.
    // Preserves queue position when shares remain.
    void onCancelPartial(OrderRef ref, std::uint32_t cancelledShares) noexcept
    {
        std::int32_t idx = liveOrders_.find(ref);
        if (idx < 0) return;
        decrementOrErase(idx, static_cast<Quantity>(cancelledShares));
    }

    // ITCH 'D' -- delete the full residual.
    void onDelete(OrderRef ref) noexcept
    {
        std::int32_t idx = liveOrders_.find(ref);
        if (idx < 0) return;
        eraseOrder(idx);
    }

    // ITCH 'E' -- `executedShares` of the order traded at its resting price.
    void onExecute(OrderRef ref, std::uint32_t executedShares) noexcept
    {
        std::int32_t idx = liveOrders_.find(ref);
        if (idx < 0) return;
        decrementOrErase(idx, static_cast<Quantity>(executedShares));
    }

    // ITCH 'C' -- like E but with an explicit print price; for the reconstructed
    // displayed book the qty bookkeeping is identical and the price informational.
    void onExecuteWithPrice(OrderRef ref, std::uint32_t executedShares,
                            std::uint32_t, char) noexcept
    {
        std::int32_t idx = liveOrders_.find(ref);
        if (idx < 0) return;
        decrementOrErase(idx, static_cast<Quantity>(executedShares));
    }

    // ITCH 'U' -- explicit Cancel(old) + Add(new)
    void onReplace(OrderRef oldRef, OrderRef newRef,
                   std::uint32_t shares, std::uint32_t itchPx) noexcept
    {
        std::int32_t idx = liveOrders_.find(oldRef);
        if (idx < 0) return;
        const Side side = liveOrders_.handle(idx)->getSide();
        eraseOrder(idx);

        const PriceTicks px = toTicks(itchPx);
        if (!inBookRange(px)) return;
        AddCommand add{
            LimitOrder{ px, TimeInForce{} },
            static_cast<OrderId>(newRef),
            static_cast<Quantity>(shares),
            side,
        };
        Command cmd = add;
        if (OrderPointer handle = book_.Process(cmd)) {
            liveOrders_.insert(newRef, handle);
        }
    }

    OrderBook&       book()       noexcept { return book_; }
    const OrderBook& book() const noexcept { return book_; }

    struct OrderInfo { Side side; PriceTicks px; };
    bool peek(OrderRef ref, OrderInfo& out) const noexcept
    {
        std::int32_t idx = liveOrders_.find(ref);
        if (idx < 0) return false;
        const OrderPointer h = liveOrders_.handle(idx);
        out = OrderInfo{ h->getSide(), h->getPrice() };
        return true;
    }

    std::optional<OrderInfo> peek(OrderRef ref) const noexcept
    {
        OrderInfo out;
        if (!peek(ref, out)) return std::nullopt;
        return out;
    }

private:
    static constexpr PriceTicks toTicks(std::uint32_t itchPx) noexcept
    {
        return static_cast<PriceTicks>(itchPx / 100u);
    }

    static constexpr bool inBookRange(PriceTicks px) noexcept
    {
        return px > 0 && static_cast<int>(px) < MAX_LEVEL;
    }

    static constexpr Side sideFromChar(char c) noexcept
    {
        return (c == 'B') ? Side::Buy : Side::Sell;
    }

    void decrementOrErase(std::int32_t idx, Quantity delta) noexcept
    {
        OrderPointer o = liveOrders_.handle(idx);
        const Quantity remaining = o->getQuantity() - delta;
        if (remaining > 0) {
            o->setQuantity(remaining);
        } else {
            eraseOrder(idx);
        }
    }

    void eraseOrder(std::int32_t idx) noexcept
    {
        Command cmd = CancelCommand{ liveOrders_.handle(idx) };
        book_.Process(cmd);
        liveOrders_.erase(idx);
    }

    OrderBook       book_;
    LiveOrderTable  liveOrders_;
};

}  // namespace hft
