#pragma once

#include <cstdint>
#include "hft/engine/oms/order.hpp"

namespace oms {

uint64_t submit(uint16_t stock_locate, Side side, uint32_t price, uint32_t qty, uint64_t parse_ns) noexcept;

}  // namespace oms
