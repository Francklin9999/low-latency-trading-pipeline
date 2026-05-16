#pragma once

#include <cstdint>
#include "hft/engine/oms/order.hpp"

// Strategy -> risk/OMS hand-off record.
struct Signal {
    std::uint16_t stock_locate;
    std::uint32_t price;
    std::uint32_t qty;
    oms::Side     side;
    std::uint64_t parse_ns;
};
