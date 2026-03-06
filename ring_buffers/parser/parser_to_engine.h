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

#include "packet.h"

#define PARSER_TO_ENGINE_SIZE (512u * 1024u)
#define PARSER_ENGINE_MASK (PARSER_TO_ENGINE_SIZE - 1)

typedef struct {
    alignas(64) ATOMIC_UINT32 next_write;
    alignas(64) ATOMIC_UINT32 next_read;
    packet_ref* data[PARSER_TO_ENGINE_SIZE];
} parser_to_engine;

// single process
extern parser_to_engine PARSER_ENGINE;
