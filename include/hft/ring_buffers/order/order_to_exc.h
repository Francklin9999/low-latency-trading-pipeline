#pragma once

#include <stdint.h>
#include <stdalign.h>

#ifdef __cplusplus
#include <atomic>
#define ATOMIC_UINT32 std::atomic<uint32_t>
#else
#include <stdatomic.h>
#define ATOMIC_UINT32 _Atomic uint32_t
#endif

#include "hft/engine/oms/order.h"

#define ORDER_TO_EXC_SIZE (512u * 1024u)
#define ORDER_TO_EXC_MASK (ORDER_TO_EXC_SIZE - 1)

typedef struct {
    alignas(64) ATOMIC_UINT32 next_write;
    alignas(64) ATOMIC_UINT32 next_read;
    order data[ORDER_TO_EXC_SIZE];
} order_to_exc;

// shared mem
extern order_to_exc* ORDER_TO_EXC;
