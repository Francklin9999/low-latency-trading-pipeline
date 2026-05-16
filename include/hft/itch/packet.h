#pragma once
#include <stdint.h>

typedef struct {
    const uint8_t *data;
    uint64_t umem_addr;
    uint64_t ts;
    uint32_t len;
} packet_ref;
