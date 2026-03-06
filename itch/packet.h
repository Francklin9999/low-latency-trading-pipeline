#pragma once
#include <stdio.h>
#include <stdint.h>

typedef struct {
    const uint8_t *data;  // pointer into AF_XDP UMEM payload
    uint64_t umem_addr;   // UMEM frame recycle token
    uint64_t ts;          // timestamp when received
    uint32_t len;
} packet_ref;
