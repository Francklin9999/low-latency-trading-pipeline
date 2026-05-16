#pragma once

#include <stdint.h>

#define WIRE_PROTOCOL_VERSION 1

// Outbound (client -> exchange).
#define WIRE_MSG_NEW    'N'   // place a new resting limit order
#define WIRE_MSG_CANCEL 'C'   // cancel a resting order at (sym,side,px)
#define WIRE_MSG_FLUSH  'F'   // mass-cancel for a symbol

// Inbound (exchange -> client).
#define WIRE_MSG_ACK    'A'

// ACK flag values.
#define WIRE_ACK_OK         0
#define WIRE_ACK_BAD_CSUM   1
#define WIRE_ACK_SEQ_GAP    2

#pragma pack(push, 1)

typedef struct {
    uint8_t  msg_type;          // WIRE_MSG_NEW / CANCEL / FLUSH
    uint8_t  side;              // 'B' or 'S'
    uint16_t symbol_id;         // ITCH stock_locate
    uint32_t qty;               // shares (0 == cancel discriminator when NEW)
    uint32_t price;             // in ticks (cents)
    uint32_t _pad0;             // keep seq 8-byte aligned
    uint64_t seq;               // monotonic per session
    uint64_t ts;                // client send timestamp (mono ns)
    uint16_t checksum;          // sum-complement of all prior bytes
    uint16_t _pad1;
} wire_order;                   // 32 bytes

typedef struct {
    uint8_t  msg_type;          // WIRE_MSG_ACK
    uint8_t  flag;              // WIRE_ACK_*
    uint16_t _pad0;
    uint32_t _pad1;
    uint64_t seq;               // echoed client seq
    uint64_t recv_ts;           // server recv timestamp
} wire_ack;                     // 24 bytes

#pragma pack(pop)

// RFC-1071-style 16-bit ones-complement checksum. Loopback boundary check.
static inline uint16_t wire_checksum(const void *buf, uint32_t len) {
    const uint8_t *b = (const uint8_t *)buf;
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2) {
        sum += (uint32_t)b[i] | ((uint32_t)b[i+1] << 8);
        if (sum > 0xFFFF) sum = (sum & 0xFFFF) + 1;
    }
    if (len & 1) {
        sum += (uint32_t)b[len - 1];
        if (sum > 0xFFFF) sum = (sum & 0xFFFF) + 1;
    }
    return (uint16_t)(~sum & 0xFFFF);
}
