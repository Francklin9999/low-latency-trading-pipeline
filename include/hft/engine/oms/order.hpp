#pragma once

#include <cstdint>

extern "C" {
    #include "hft/engine/oms/order.h"
}

namespace oms {

enum class Side : uint8_t {
    BUY  = 'B',
    SELL = 'S'
};

}  // namespace oms
