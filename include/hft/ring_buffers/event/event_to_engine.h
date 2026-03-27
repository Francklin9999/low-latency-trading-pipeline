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

#include "hft/engine/events.h"

#define EVENT_TO_ENGINE_SIZE (512u * 1024u)
#define EVENT_ENGINE_MASK (EVENT_TO_ENGINE_SIZE - 1)

typedef struct {
    alignas(64) ATOMIC_UINT32 next_write;
    alignas(64) ATOMIC_UINT32 next_read;
    event data[EVENT_TO_ENGINE_SIZE];
} event_to_engine;

// shared mem
extern event_to_engine* EVENT_ENGINE;
