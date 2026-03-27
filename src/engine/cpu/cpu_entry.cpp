#include <immintrin.h>
#include <cstdint>
#include "hft/engine/cpu/cpu_entry.hpp"

namespace cpu_op {

constexpr char BUY_CHAR = 'B';

std::uint64_t last_ts_cancel[NUMBER_OF_DISPATCHERS]{};
std::uint64_t last_ts_delete[NUMBER_OF_DISPATCHERS]{};

void run_add_order(AddOrderBatch& batch) {
    int i = 0;
    int n = batch.n;

    for (i = 0; i + 8 <= n; i += 8) {
        __m256i price = _mm256_loadu_si256((__m256i*) &batch.price[i]);
        __m256i qty = _mm256_loadu_si256((__m256i*) &batch.qty[i]);
        __m256i notional = _mm256_mullo_epi32(price, qty);
        _mm256_storeu_si256((__m256i*) &batch.notional[i], notional);
    }
    for (; i < n; ++i)
        batch.notional[i] = batch.price[i] * batch.qty[i];

    const __m256i buy = _mm256_set1_epi32(static_cast<int>(BUY_CHAR));
    for (i = 0; i + 8 <= n; i += 8) {
        __m256i qty = _mm256_loadu_si256((__m256i*) &batch.qty[i]);
        __m256i side32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i*) &batch.side[i]));
        __m256i bmask = _mm256_cmpeq_epi32(side32, buy);
        _mm256_storeu_si256((__m256i*) &batch.bid_qty[i], _mm256_and_si256(qty, bmask));
        _mm256_storeu_si256((__m256i*) &batch.ask_qty[i], _mm256_andnot_si256(bmask, qty));
    }
    for (; i < n; ++i) {
        batch.bid_qty[i] = batch.side[i] == BUY_CHAR ? batch.qty[i] : 0;
        batch.ask_qty[i] = batch.side[i] != BUY_CHAR ? batch.qty[i] : 0;
    }
}

void run_add_order_mpi(AddOrderMpidBatch& batch) {
    int i = 0;
    int n = batch.n;

    for (i = 0; i + 8 <= n; i += 8) {
        __m256i price = _mm256_loadu_si256((__m256i*) &batch.price[i]);
        __m256i qty = _mm256_loadu_si256((__m256i*) &batch.qty[i]);
        __m256i notional = _mm256_mullo_epi32(price, qty);
        _mm256_storeu_si256((__m256i*) &batch.notional[i], notional);
    }
    for (; i < n; ++i)
        batch.notional[i] = batch.price[i] * batch.qty[i];

    const __m256i buy = _mm256_set1_epi32(static_cast<int>(BUY_CHAR));
    for (i = 0; i + 8 <= n; i += 8) {
        __m256i qty = _mm256_loadu_si256((__m256i*) &batch.qty[i]);
        __m256i side32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i*) &batch.side[i]));
        __m256i bmask = _mm256_cmpeq_epi32(side32, buy);
        _mm256_storeu_si256((__m256i*) &batch.bid_qty[i], _mm256_and_si256(qty, bmask));
        _mm256_storeu_si256((__m256i*) &batch.ask_qty[i], _mm256_andnot_si256(bmask, qty));
    }
    for (; i < n; ++i) {
        batch.bid_qty[i] = batch.side[i] == BUY_CHAR ? batch.qty[i] : 0;
        batch.ask_qty[i] = batch.side[i] != BUY_CHAR ? batch.qty[i] : 0;
    }
}

void run_modify(OrderModifyBatch& batch) {
    int i = 0;
    int n = batch.n;

    for (i = 0; i + 8 <= n; i += 8) {
        __m256i price = _mm256_loadu_si256((__m256i*) &batch.price[i]);
        __m256i qty = _mm256_loadu_si256((__m256i*) &batch.qty[i]);
        __m256i notional = _mm256_mullo_epi32(price, qty);
        _mm256_storeu_si256((__m256i*) &batch.notional[i], notional);
    }
    for (; i < n; ++i)
        batch.notional[i] = batch.price[i] * batch.qty[i];
}

void run_execute(OrderExecutedPriceBatch& batch) {
    int i = 0;
    int n = batch.n;

    for (i = 0; i + 8 <= n; i += 8) {
        __m256i price = _mm256_loadu_si256((__m256i*) &batch.price[i]);
        __m256i qty = _mm256_loadu_si256((__m256i*) &batch.qty[i]);
        __m256i notional = _mm256_mullo_epi32(price, qty);
        _mm256_storeu_si256((__m256i*) &batch.notional[i], notional);
    }
    for (; i < n; ++i)
        batch.notional[i] = batch.price[i] * batch.qty[i];
}

void run_cancel(OrderCancelBatch& batch, int lane) {
    int n = batch.n;
    if (n == 0) return;

    batch.ts_delta[0] = batch.ts[0] - last_ts_cancel[lane];
    int i = 1;
    for (; i + 4 <= n; i += 4) {
        __m256i cur = _mm256_loadu_si256((__m256i*) &batch.ts[i]);
        __m256i prev = _mm256_loadu_si256((__m256i*) &batch.ts[i - 1]);
        __m256i delta = _mm256_sub_epi64(cur, prev);
        _mm256_storeu_si256((__m256i*) &batch.ts_delta[i], delta);
    }
    for (; i < n; ++i)
        batch.ts_delta[i] = batch.ts[i] - batch.ts[i - 1];
    last_ts_cancel[lane] = batch.ts[n - 1];
}

void run_delete(OrderDeleteBatch& batch, int lane) {
    int n = batch.n;
    if (n == 0) return;

    batch.ts_delta[0] = batch.ts[0] - last_ts_delete[lane];
    int i = 1;
    for (; i + 4 <= n; i += 4) {
        __m256i cur = _mm256_loadu_si256((__m256i*) &batch.ts[i]);
        __m256i prev = _mm256_loadu_si256((__m256i*) &batch.ts[i - 1]);
        __m256i delta = _mm256_sub_epi64(cur, prev);
        _mm256_storeu_si256((__m256i*) &batch.ts_delta[i], delta);
    }
    for (; i < n; ++i)
        batch.ts_delta[i] = batch.ts[i] - batch.ts[i - 1];
    last_ts_delete[lane] = batch.ts[n - 1];
}

void run_replace(OrderReplaceBatch& batch) {
    int i = 0;
    int n = batch.n;

    for (i = 0; i + 8 <= n; i += 8) {
        __m256i price = _mm256_loadu_si256((__m256i*) &batch.price[i]);
        __m256i qty = _mm256_loadu_si256((__m256i*) &batch.qty[i]);
        __m256i notional = _mm256_mullo_epi32(price, qty);
        _mm256_storeu_si256((__m256i*) &batch.notional[i], notional);
    }
    for (; i < n; ++i)
        batch.notional[i] = batch.price[i] * batch.qty[i];

    const __m256i buy = _mm256_set1_epi32(static_cast<int>(BUY_CHAR));
    for (i = 0; i + 8 <= n; i += 8) {
        __m256i qty = _mm256_loadu_si256((__m256i*) &batch.qty[i]);
        __m256i side32 = _mm256_cvtepu8_epi32(_mm_loadl_epi64((__m128i*) &batch.side[i]));
        __m256i bmask = _mm256_cmpeq_epi32(side32, buy);
        _mm256_storeu_si256((__m256i*) &batch.bid_qty[i], _mm256_and_si256(qty, bmask));
        _mm256_storeu_si256((__m256i*) &batch.ask_qty[i], _mm256_andnot_si256(bmask, qty));
    }
    for (; i < n; ++i) {
        batch.bid_qty[i] = batch.side[i] == BUY_CHAR ? batch.qty[i] : 0;
        batch.ask_qty[i] = batch.side[i] != BUY_CHAR ? batch.qty[i] : 0;
    }
}

void run_lane(BatchBuffer& batch, int lane) {
    run_add_order(batch.addOrder[lane]);
    run_add_order_mpi(batch.addOrderMpid[lane]);
    run_modify(batch.orderModify[lane]);
    run_execute(batch.orderExecPrice[lane]);
    run_cancel(batch.orderCancel[lane], lane);
    run_delete(batch.orderDelete[lane], lane);
    run_replace(batch.orderReplace[lane]);
}

}