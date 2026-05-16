#pragma once

#include <stdint.h>

typedef struct __attribute__((aligned(64))) {
    uint64_t order_id;          // 8
    uint64_t ts;                // 8  reserved; unread downstream (order_sender uses send_ns)
    uint32_t price;             // 4
    uint32_t qty;               // 4
    uint16_t stock_locate;      // 2
    uint8_t side;               // 1
    uint8_t _pad0[5];           // 5
    uint64_t parse_ns;          // 8  stamped at ITCH parse time
    uint64_t send_ns;           // 8  stamped at ring-buffer dequeue (order_sender)
    uint8_t _pad[16];           // 16
} order;                        // Total 64
