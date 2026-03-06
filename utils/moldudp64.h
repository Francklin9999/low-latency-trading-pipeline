#pragma once

#include <stdint.h>

#define MTU_MAX_SIZE 1500

// 1500 (MTU) - 28 header size - MOLDUDP header
#define PACKET_SIZE (1500 - 28 - sizeof(mold_udp64_header))


#pragma pack(push, 1)
typedef struct {
    unsigned char session[10];
    uint64_t sequence_number;
    uint16_t message_count;
} mold_udp64_header;
#pragma pack(pop)
