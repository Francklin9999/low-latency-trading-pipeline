#pragma once

#include <cstdint>

extern "C" {
    #include "order.h"
}

enum class Side : uint8_t {
    BUY  = 'B',
    SELL = 'S'
};
