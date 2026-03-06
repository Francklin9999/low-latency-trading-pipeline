#pragma once
#include <stdint.h>
#include "structs.h"
#include "events.h"

#include <arpa/inet.h>

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    #ifndef be16toh
        #define be16toh(x) __builtin_bswap16(x)
    #endif
    #ifndef be32toh
        #define be32toh(x) __builtin_bswap32(x)
    #endif
    #ifndef be64toh
        #define be64toh(x) __builtin_bswap64(x)
    #endif
#else
    #ifndef be16toh
        #define be16toh(x) (x)
    #endif
    #ifndef be32toh
        #define be32toh(x) (x)
    #endif
    #ifndef be64toh
        #define be64toh(x) (x)
    #endif
#endif

typedef int (*itch_handler_fn) (uint8_t *msg, event *out);

extern itch_handler_fn dispatch_table[256];

inline uint64_t get_timestamp(const uint8_t* ts) {
    return ((uint64_t)ts[0] << 40) |
           ((uint64_t)ts[1] << 32) |
           ((uint64_t)ts[2] << 24) |
           ((uint64_t)ts[3] << 16) |
           ((uint64_t)ts[4] << 8) |
           ((uint64_t)ts[5]);
}

int handle_system_event(uint8_t* msg, event *out);
int handle_stock_directory(uint8_t* msg, event *out);
int handle_stock_trading_action(uint8_t* msg, event *out);
int handle_reg_sho(uint8_t* msg, event *out);
int handle_mkt_participant_pos(uint8_t* msg, event *out);
int handle_mwcb_decline_level(uint8_t* msg, event *out);
int handle_mwcb_status(uint8_t* msg, event *out);
int handle_ipo_quoting_period_update(uint8_t* msg, event *out);
int handle_luld_auction_collar(uint8_t* msg, event *out);
int handle_operational_halt(uint8_t* msg, event *out);
int handle_add_order(uint8_t* msg, event *out);
int handle_add_order_mpid(uint8_t* msg, event *out);
int handle_order_modify(uint8_t* msg, event *out);
int handle_order_executed_price(uint8_t* msg, event *out);
int handle_order_cancel(uint8_t* msg, event *out);
int handle_order_delete(uint8_t* msg, event *out);
int handle_order_replace(uint8_t* msg, event *out);
int handle_non_cross_trade(uint8_t* msg, event *out);
int handle_cross_trade(uint8_t* msg, event *out);
int handle_broken_trade(uint8_t* msg, event *out);
int handle_noii(uint8_t* msg, event *out);
int handle_rpii(uint8_t* msg, event *out);
int handle_dlcr(uint8_t* msg, event *out);

