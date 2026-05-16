#pragma once

#include <stdint.h>
#include <stdalign.h>

#include "hft/engine/oms/order.h"

#define ORDER_TO_EXC_SIZE (512u * 1024u)
#define ORDER_TO_EXC_MASK (ORDER_TO_EXC_SIZE - 1)

typedef struct {
    alignas(64) uint32_t next_write;
    alignas(64) uint32_t next_read;
    order data[ORDER_TO_EXC_SIZE];
} order_to_exc;

#ifdef __cplusplus
extern "C" order_to_exc* ORDER_TO_EXC;
#else
extern order_to_exc* ORDER_TO_EXC;
#endif
