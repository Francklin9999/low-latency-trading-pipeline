#pragma once

#include <cstdint>
#include "batch_sizes.hpp"

struct alignas(64) AddOrderBatch {
  std::uint32_t n;

  alignas(64) std::uint64_t oid[BATCH_MAX];
  alignas(64) std::uint64_t ts[BATCH_MAX];

  alignas(64) std::uint32_t price[BATCH_MAX];
  alignas(64) std::uint32_t qty[BATCH_MAX];

  alignas(64) std::uint16_t loc[BATCH_MAX];
  alignas(64) std::uint8_t side[BATCH_MAX];

  alignas(64) std::uint32_t notional[BATCH_MAX];
  alignas(64) std::uint32_t bid_qty[BATCH_MAX];
  alignas(64) std::uint32_t ask_qty[BATCH_MAX];
};

struct alignas(64) AddOrderMpidBatch {
  std::uint32_t n;

  alignas(64) std::uint64_t oid[BATCH_MAX];
  alignas(64) std::uint64_t ts[BATCH_MAX];

  alignas(64) std::uint32_t price[BATCH_MAX];
  alignas(64) std::uint32_t qty[BATCH_MAX];

  alignas(64) std::uint16_t loc[BATCH_MAX];
  alignas(64) std::uint8_t side[BATCH_MAX];

  alignas(64) std::uint32_t notional[BATCH_MAX];
  alignas(64) std::uint32_t bid_qty[BATCH_MAX];
  alignas(64) std::uint32_t ask_qty[BATCH_MAX];
};

struct alignas(64) OrderModifyBatch {
  std::uint32_t n;

  alignas(64) std::uint64_t oid[BATCH_MAX];
  alignas(64) std::uint64_t ts[BATCH_MAX];

  alignas(64) std::uint32_t qty[BATCH_MAX];
  alignas(64) std::uint32_t price[BATCH_MAX];

  alignas(64) std::uint16_t loc[BATCH_MAX];

  alignas(64) std::uint32_t notional[BATCH_MAX];
};

struct alignas(64) OrderExecutedPriceBatch {
  std::uint32_t n;

  alignas(64) std::uint64_t oid[BATCH_MAX];
  alignas(64) std::uint64_t ts[BATCH_MAX];

  alignas(64) std::uint32_t qty[BATCH_MAX];
  alignas(64) std::uint32_t price[BATCH_MAX];

  alignas(64) std::uint16_t loc[BATCH_MAX];

  alignas(64) std::uint32_t notional[BATCH_MAX];
};

struct alignas(64) OrderCancelBatch {
  std::uint32_t n;

  alignas(64) std::uint64_t oid[BATCH_MAX];
  alignas(64) std::uint64_t ts[BATCH_MAX];

  alignas(64) std::uint32_t qty[BATCH_MAX];

  alignas(64) std::uint16_t loc[BATCH_MAX];
  alignas(64) std::uint16_t ts_delta[BATCH_MAX];
};

struct alignas(64) OrderDeleteBatch {
  std::uint32_t n;

  alignas(64) std::uint64_t oid[BATCH_MAX];
  alignas(64) std::uint64_t ts[BATCH_MAX];

  alignas(64) std::uint16_t loc[BATCH_MAX];
  alignas(64) std::uint16_t ts_delta[BATCH_MAX];
};

struct alignas(64) OrderReplaceBatch {
  std::uint32_t n;

  alignas(64) std::uint64_t new_oid[BATCH_MAX];
  alignas(64) std::uint64_t ts[BATCH_MAX];

  alignas(64) std::uint32_t price[BATCH_MAX];
  alignas(64) std::uint32_t qty[BATCH_MAX];

  alignas(64) std::uint16_t loc[BATCH_MAX];
  alignas(64) std::uint8_t side[BATCH_MAX];

  alignas(64) std::uint32_t notional[BATCH_MAX];
  alignas(64) std::uint32_t bid_qty[BATCH_MAX];
  alignas(64) std::uint32_t ask_qty[BATCH_MAX];
};
