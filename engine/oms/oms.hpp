#pragma once

#include <atomic>
#include <cstdint>
#include "../risk/risk.hpp"
#include "order.hpp"
#include "../dispatcher.hpp"

namespace oms {

uint64_t submit(uint16_t stock_locate, Side side, uint32_t price, uint32_t qty, uint64_t parse_ns) noexcept;

uint32_t drain(order* buf, uint32_t max_orders) noexcept;

}