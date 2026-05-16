#pragma once
#include <stdint.h>


typedef struct {
    uint64_t ts;
    uint64_t order_id;
    uint64_t aux;

    uint32_t price;
    uint32_t qty;

    uint16_t stock_locate;

    uint8_t type;
    uint8_t side;
} event;
