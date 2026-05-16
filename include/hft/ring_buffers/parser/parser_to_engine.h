#pragma once

#include <stdint.h>
#include <stdalign.h>

#include "hft/itch/packet.h"

#define PARSER_TO_ENGINE_SIZE (512u * 1024u)
#define PARSER_ENGINE_MASK (PARSER_TO_ENGINE_SIZE - 1)

typedef struct {
    alignas(64) uint32_t next_write;
    alignas(64) uint32_t next_read;
    packet_ref* data[PARSER_TO_ENGINE_SIZE];
} parser_to_engine;

#ifdef __cplusplus
extern "C" parser_to_engine PARSER_ENGINE;
#else
extern parser_to_engine PARSER_ENGINE;
#endif
