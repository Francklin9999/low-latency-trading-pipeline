#include <stdint.h>
#include "itch_handler.h"
#include "structs.h"
#include "events.h"
#include "../utils/utils.h"

itch_handler_fn dispatch_table[256] = {
    ['S'] = (itch_handler_fn) handle_system_event,
    ['R'] = (itch_handler_fn) handle_stock_directory,
    ['H'] = (itch_handler_fn) handle_stock_trading_action,
    ['Y'] = (itch_handler_fn) handle_reg_sho,
    ['L'] = (itch_handler_fn) handle_mkt_participant_pos,
    ['V'] = (itch_handler_fn) handle_mwcb_decline_level,
    ['W'] = (itch_handler_fn) handle_mwcb_status,
    ['K'] = (itch_handler_fn) handle_ipo_quoting_period_update,
    ['J'] = (itch_handler_fn) handle_luld_auction_collar,
    ['h'] = (itch_handler_fn) handle_operational_halt,
    ['A'] = (itch_handler_fn) handle_add_order,
    ['F'] = (itch_handler_fn) handle_add_order_mpid,
    ['E'] = (itch_handler_fn) handle_order_modify,
    ['C'] = (itch_handler_fn) handle_order_executed_price,
    ['X'] = (itch_handler_fn) handle_order_cancel,
    ['D'] = (itch_handler_fn) handle_order_delete,
    ['U'] = (itch_handler_fn) handle_order_replace,
    ['P'] = (itch_handler_fn) handle_non_cross_trade,
    ['Q'] = (itch_handler_fn) handle_cross_trade,
    ['B'] = (itch_handler_fn) handle_broken_trade,
    ['I'] = (itch_handler_fn) handle_noii,
    ['N'] = (itch_handler_fn) handle_rpii,
    ['O'] = (itch_handler_fn) handle_dlcr,
};

int handle_system_event(uint8_t* msg, event *out) { // S
    return -1;
}

int handle_stock_directory(uint8_t* msg, event *out) { // R
    return -1;
}

int handle_stock_trading_action(uint8_t* msg, event *out) { // H
    return -1;
}

int handle_reg_sho(uint8_t* msg, event *out) { // Y
    return -1;
}

int handle_mkt_participant_pos(uint8_t* msg, event *out) { // L
    return -1;
}

int handle_mwcb_decline_level(uint8_t* msg, event *out) { // V
    return -1;
}

int handle_mwcb_status(uint8_t* msg, event *out) { // W
    return -1;
}

int handle_ipo_quoting_period_update(uint8_t* msg, event *out) { // K
    return -1;
}

int handle_luld_auction_collar(uint8_t* msg, event *out) { // J
    return -1;
}

int handle_operational_halt(uint8_t* msg, event *out) { // h
    return -1;
}

int handle_add_order(uint8_t* msg, event *out) { // A
    itch_add_order_t* ptr = (itch_add_order_t*) msg;
    out->ts = local_ns();
    out->order_id = be64toh(ptr->order_reference_number);
    out->price = be32toh(ptr->price);
    out->qty = be32toh(ptr->shares);
    out->stock_locate = ptr->header.stock_locate;
    out->type = ptr->header.message_type;
    out->side = ptr->buy_sell_indicator;
    return 0;
}

int handle_add_order_mpid(uint8_t* msg, event *out) { // F
    itch_add_order_mpid_t* ptr = (itch_add_order_mpid_t*) msg;
    out->ts = local_ns();
    out->order_id = be64toh(ptr->order_reference_number);
    out->price = be32toh(ptr->price);
    out->qty = be32toh(ptr->shares);
    out->stock_locate = ptr->header.stock_locate;
    out->type = ptr->header.message_type;
    out->side = ptr->buy_sell_indicator;
    return 0;
}

int handle_order_modify(uint8_t* msg, event *out) { // E
    itch_order_modify_t* ptr = (itch_order_modify_t*) msg;
    out->ts = local_ns();
    out->order_id = be64toh(ptr->order_reference_number);
    out->price = 0;
    out->qty = be32toh(ptr->executed_shares);
    out->stock_locate = ptr->header.stock_locate;
    out->type = ptr->header.message_type;
    out->side = 0;
    return 0;
}

int handle_order_executed_price(uint8_t* msg, event *out) { // C
    itch_order_executed_price_t* ptr = (itch_order_executed_price_t*) msg;
    out->ts = local_ns();
    out->order_id = be64toh(ptr->order_reference_number);
    out->price = be32toh(ptr->execution_price);
    out->qty = be32toh(ptr->executed_shares);
    out->stock_locate = ptr->header.stock_locate;
    out->type = ptr->header.message_type;
    out->side = 0;
    return 0;
}

int handle_order_cancel(uint8_t* msg, event *out) { // X
    itch_order_cancel_t* ptr = (itch_order_cancel_t*) msg;
    out->ts = local_ns();
    out->order_id = be64toh(ptr->order_reference_number);
    out->price = 0;
    out->qty = be32toh(ptr->cancelled_shares);
    out->stock_locate = ptr->header.stock_locate;
    out->type = ptr->header.message_type;
    out->side = 0;
    return 0;
}

int handle_order_delete(uint8_t* msg, event *out) { // D
    itch_order_delete_t* ptr = (itch_order_delete_t*) msg;
    out->ts = local_ns();
    out->order_id = be64toh(ptr->order_reference_number);
    out->price = 0;
    out->qty = 0;
    out->stock_locate = ptr->header.stock_locate;
    out->type = ptr->header.message_type;
    out->side = 0;
    return 0;
}

int handle_order_replace(uint8_t* msg, event *out) { // U
    itch_order_replace_t* ptr = (itch_order_replace_t*) msg;
    out->ts = local_ns();
    out->order_id = be64toh(ptr->new_order_reference_number);
    out->price = be32toh(ptr->price);
    out->qty = be32toh(ptr->shares);
    out->stock_locate = ptr->header.stock_locate;
    out->type = ptr->header.message_type;
    out->side = 0;
    return 0;
}

int handle_non_cross_trade(uint8_t* msg, event *out) { // P
    return -1;
}

int handle_cross_trade(uint8_t* msg, event *out) { // Q
    return -1;
}

int handle_broken_trade(uint8_t* msg, event *out) { // B
    return -1;
}

int handle_noii(uint8_t* msg, event *out) { // I
    return -1;
}

int handle_rpii(uint8_t* msg, event *out) { // N
    return -1;
}

int handle_dlcr(uint8_t* msg, event *out) { // O
    return -1;
}
