#include "hft/engine/strategy/imbalance_strat.hpp"
#include <immintrin.h>

#ifndef NUMBER_OF_DISPATCHERS
#define NUMBER_OF_DISPATCHERS 2
#endif

namespace imbalance {

struct ChannelState {
    alignas(64) uint32_t bid[65536];
    alignas(64) uint32_t ask[65536];
    alignas(64) uint32_t price[65536];
    alignas(64) uint64_t ts[65536];

    uint16_t touched[BATCH_MAX * 2];
    uint32_t n_touched = 0;

    void reset() noexcept {
        for (uint32_t i = 0; i < n_touched; ++i) {
            const uint16_t loc = touched[i];
            bid[loc] = 0;
            ask[loc] = 0;
            price[loc] = 0;
            ts[loc] = 0;
        }
        n_touched = 0;
    }
};

alignas(64) static ChannelState state[NUMBER_OF_DISPATCHERS];

template<typename Batch>
static void bin_batch(ChannelState& s, const Batch& batch) noexcept {
    for (uint32_t i = 0; i < batch.n; ++i) {
        const uint16_t loc = batch.loc[i];

        if (s.bid[loc] == 0 && s.ask[loc] == 0) {
            s.touched[s.n_touched++] = loc;
            s.ts[loc] = batch.ts[i];
        }

        s.bid[loc] += batch.bid_qty[i];
        s.ask[loc] += batch.ask_qty[i];
        s.price[loc] = batch.price[i];
    }
}

int evaluate(const AddOrderBatch& addOrder,
    const AddOrderMpidBatch& addOrderMpid,
    Signal* out, int max_out, int lane) noexcept
{
    ChannelState* const s = &state[lane];
    bin_batch(*s, addOrder);
    bin_batch(*s, addOrderMpid);

    int count = 0;
    uint32_t i = 0;

    const __m256i thresh = _mm256_set1_epi64x(
        static_cast<int64_t>(IMBAL_MIN_TOTAL_QTY) - 1);

    for (; i + 4 <= s->n_touched && count + 4 <= max_out; i += 4) {
        const uint16_t* locs = &s->touched[i];

        __m256i bid_v = _mm256_set_epi64x(
            s->bid[locs[3]], s->bid[locs[2]],
            s->bid[locs[1]], s->bid[locs[0]]);
        __m256i ask_v = _mm256_set_epi64x(
            s->ask[locs[3]], s->ask[locs[2]],
            s->ask[locs[1]], s->ask[locs[0]]);
        __m256i total_v = _mm256_add_epi64(bid_v, ask_v);

        const int pmask = _mm256_movemask_epi8(
            _mm256_cmpgt_epi64(total_v, thresh));
        if (pmask == 0) continue;

        alignas(32) uint64_t bids[4], asks[4], totals[4];
        _mm256_store_si256((__m256i*)bids, bid_v);
        _mm256_store_si256((__m256i*)asks, ask_v);
        _mm256_store_si256((__m256i*)totals, total_v);

        for (int j = 0; j < 4 && count < max_out; ++j) {
            if (!((pmask >> (j * 8)) & 1)) continue;

            const uint16_t loc = locs[j];
            const float tf = static_cast<float>(totals[j]);
            const float bid_ratio = static_cast<float>(bids[j]) / tf;
            const float ask_ratio = static_cast<float>(asks[j]) / tf;

            if (bid_ratio >= IMBAL_BUY_THRESHOLD) {
                out[count++] = { loc, s->price[loc], IMBAL_ORDER_QTY, Side::BUY, s->ts[loc] };
            } else if (ask_ratio >= IMBAL_SELL_THRESHOLD) {
                out[count++] = { loc, s->price[loc], IMBAL_ORDER_QTY, Side::SELL, s->ts[loc] };
            }
        }
    }

    for (; i < s->n_touched && count < max_out; ++i) {
        const uint16_t loc = s->touched[i];
        const uint64_t total = s->bid[loc] + s->ask[loc];
        if (total < IMBAL_MIN_TOTAL_QTY) continue;

        const float tf = static_cast<float>(total);
        const float bid_ratio = static_cast<float>(s->bid[loc]) / tf;
        const float ask_ratio = static_cast<float>(s->ask[loc]) / tf;

        if (bid_ratio >= IMBAL_BUY_THRESHOLD) {
            out[count++] = { loc, s->price[loc], IMBAL_ORDER_QTY, Side::BUY, s->ts[loc] };
        } else if (ask_ratio >= IMBAL_SELL_THRESHOLD) {
            out[count++] = { loc, s->price[loc], IMBAL_ORDER_QTY, Side::SELL, s->ts[loc] };
        }
    }

    s->reset();
    return count;
}

}
