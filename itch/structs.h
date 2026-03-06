#pragma once
#include <stdint.h>

// Common header
#pragma pack(push, 1)
typedef struct {
    char     message_type;                     //  1 byte  | 'S','R','H','Y','L','V','W','K','J','h','A','F','E','C','X','D','U','P','Q','B','I','N','O'
    uint16_t stock_locate;                     //  2 bytes | 0-65535
    uint16_t tracking_number;                  //  2 bytes | 0-65535
    uint8_t  timestamp[6];                     //  6 bytes | nanoseconds since midnight
} itch_header_t;                               // 11 bytes total
#pragma pack(pop)

// System messages
#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'S'
    char          event_code;                  //  1 byte  | 'O','S','Q','M','E','C'
} itch_system_event_t;                         // 12 bytes total
#pragma pack(pop)

// Stock structs
#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'R'
    char     stock[8];                         //  8 bytes | right-padded with spaces
    char     market_category;                  //  1 byte  | 'Q','G','S','N','A','P','Z',' '
    char     financial_status;                 //  1 byte  | 'N','D','E','Q','S','G','H','J','K',' '
    uint32_t round_lot_size;                   //  4 bytes | integer
    char     round_lots;                       //  1 byte  | 'Y' or 'N'
    char     issue_classification;             //  1 byte  | 'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z'
    char     issue_sub_type[2];                //  2 bytes | e.g. 'A ','AI','B ','C ','CB','CF',...
    char     authenticity;                     //  1 byte  | 'P' (live) or 'T' (test)
    char     short_sale_threshold_indicator;   //  1 byte  | 'Y','N',' '
    char     ipo_flag;                         //  1 byte  | 'Y','N',' '
    char     luld_reference_price_tier;        //  1 byte  | ' ','1','2'
    char     etp_flag;                         //  1 byte  | 'Y','N',' '
    uint32_t etp_leverage_factor;              //  4 bytes | integer
    char     inverse_indicator;                //  1 byte  | 'Y' or 'N'
} itch_stock_directory_t;                      // 39 bytes total
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'H'
    char     stock[8];                         //  8 bytes | right-padded with spaces
    char     trading_state;                    //  1 byte  | 'H','P','Q','T'
    char     reserved;                         //  1 byte  | always ' '
    char     reason[4];                        //  4 bytes | e.g. 'MWC1','IPO1','LUDP',...
} itch_stock_trading_action_t;                 // 25 bytes total
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'Y'
    char     stock[8];                         //  8 bytes | right-padded with spaces
    char     reg_sho_action;                   //  1 byte  | '0','1','2'
} itch_reg_sho_t;                              // 20 bytes total
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'L'
    char     mpid[4];                          //  4 bytes | market participant ID
    char     stock[8];                         //  8 bytes | right-padded with spaces
    char     primary_market_maker;             //  1 byte  | 'Y' or 'N'
    char     market_maker_mode;                //  1 byte  | 'N','P','S','A'
    char     market_participant_state;         //  1 byte  | 'A','E','W','S','D'
} itch_mkt_participant_pos_t;                  // 26 bytes total
#pragma pack(pop)

// MWCB messages (market-wide circuit breakers)
#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'V'
    uint64_t level_1;                          //  8 bytes | Price(8)
    uint64_t level_2;                          //  8 bytes | Price(8)
    uint64_t level_3;                          //  8 bytes | Price(8)
} itch_mwcb_decline_level_t;                   // 35 bytes total
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'W'
    char     breached_level;                   //  1 byte  | '1','2','3'
} itch_mwcb_status_t;                          // 12 bytes total
#pragma pack(pop)

// IPO, LULD, Halt messages
#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'K'
    char     stock[8];                         //  8 bytes | right-padded with spaces
    uint32_t ipo_quotation_release_time;       //  4 bytes | seconds after midnight
    char     ipo_quotation_release_qualifier;  //  1 byte  | 'A','C'
    uint32_t ipo_price;                        //  4 bytes | Price(4)
} itch_ipo_quoting_period_update_t;            // 28 bytes total
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'J'
    char     stock[8];                         //  8 bytes | right-padded with spaces
    uint32_t auction_collar_reference_price;   //  4 bytes | Price(4)
    uint32_t upper_auction_collar_price;       //  4 bytes | Price(4)
    uint32_t lower_auction_collar_price;       //  4 bytes | Price(4)
    uint32_t auction_collar_extension;         //  4 bytes | integer
} itch_luld_auction_collar_t;                  // 35 bytes total
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'h'
    char     stock[8];                         //  8 bytes | right-padded with spaces
    char     market_code;                      //  1 byte  | 'Q','B','X'
    char     operational_halt_action;          //  1 byte  | 'H' (halted) or 'T' (resumed)
} itch_operational_halt_t;                     // 21 bytes total
#pragma pack(pop)

// Order messages
#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'A'
    uint64_t order_reference_number;           //  8 bytes | 0-999999999999999999
    char     buy_sell_indicator;               //  1 byte  | 'B' (buy) or 'S' (sell)
    uint32_t shares;                           //  4 bytes | integer
    char     stock[8];                         //  8 bytes | right-padded with spaces
    uint32_t price;                            //  4 bytes | Price(4)
} itch_add_order_t;                            // 36 bytes total
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'F'
    uint64_t order_reference_number;           //  8 bytes | 0-999999999999999999
    char     buy_sell_indicator;               //  1 byte  | 'B' (buy) or 'S' (sell)
    uint32_t shares;                           //  4 bytes | integer
    char     stock[8];                         //  8 bytes | right-padded with spaces
    uint32_t price;                            //  4 bytes | Price(4)
    char     attribution[4];                   //  4 bytes | MPID
} itch_add_order_mpid_t;                       // 40 bytes total
#pragma pack(pop)

// Order modify messages
#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'E'
    uint64_t order_reference_number;           //  8 bytes | reference to original order
    uint32_t executed_shares;                  //  4 bytes | integer
    uint64_t match_number;                     //  8 bytes | 1-999999999999999999
} itch_order_modify_t;                         // 31 bytes total
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'C'
    uint64_t order_reference_number;           //  8 bytes | reference to original order
    uint32_t executed_shares;                  //  4 bytes | integer
    uint64_t match_number;                     //  8 bytes | 1-999999999999999999
    char     printable;                        //  1 byte  | 'Y' (printable) or 'N' (non-printable)
    uint32_t execution_price;                  //  4 bytes | Price(4)
} itch_order_executed_price_t;                 // 36 bytes total
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'X'
    uint64_t order_reference_number;           //  8 bytes | reference to original order
    uint32_t cancelled_shares;                 //  4 bytes | integer
} itch_order_cancel_t;                         // 23 bytes total
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'D'
    uint64_t order_reference_number;           //  8 bytes | reference to original order
} itch_order_delete_t;                         // 19 bytes total
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'U'
    uint64_t original_order_reference_number;  //  8 bytes | reference to original order
    uint64_t new_order_reference_number;       //  8 bytes | new reference number
    uint32_t shares;                           //  4 bytes | integer
    uint32_t price;                            //  4 bytes | Price(4)
} itch_order_replace_t;                        // 35 bytes total
#pragma pack(pop)

// Trade messages
#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'P'
    uint64_t order_reference_number;           //  8 bytes | reference to original order
    char     buy_sell_indicator;               //  1 byte  | 'B' (buy) or 'S' (sell)
    uint32_t shares;                           //  4 bytes | integer
    char     stock[8];                         //  8 bytes | right-padded with spaces
    uint32_t price;                            //  4 bytes | Price(4)
    uint64_t match_number;                     //  8 bytes | 1-999999999999999999
} itch_non_cross_trade_t;                      // 44 bytes total
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'Q'
    uint64_t shares;                           //  8 bytes | integer
    char     stock[8];                         //  8 bytes | right-padded with spaces
    uint32_t cross_price;                      //  4 bytes | Price(4)
    uint64_t match_number;                     //  8 bytes | 1-999999999999999999
    char     cross_type;                       //  1 byte  | 'O' (opening), 'C' (closing), 'H' (halted/IPO)
} itch_cross_trade_t;                          // 40 bytes total
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'B'
    uint64_t match_number;                     //  8 bytes | 1-999999999999999999
} itch_broken_trade_t;                         // 19 bytes total
#pragma pack(pop)

// Auction messages
#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'I'
    uint64_t paired_shares;                    //  8 bytes | integer
    uint64_t imbalance_shares;                 //  8 bytes | integer
    char     imbalance_direction;              //  1 byte  | 'B' (buy), 'S' (sell), 'N' (no imbalance), 'O' (insufficient orders)
    char     stock[8];                         //  8 bytes | right-padded with spaces
    uint32_t far_price;                        //  4 bytes | Price(4)
    uint32_t near_price;                       //  4 bytes | Price(4)
    uint32_t current_reference_price;          //  4 bytes | Price(4)
    char     cross_type;                       //  1 byte  | 'O' (opening), 'C' (closing), 'H' (halted/IPO), 'A' (after-hours)
    char     price_variation_indicator;        //  1 byte  | 'L','1','2','3','4','5','6','7','8','9','A','B','C',' '
} itch_noii_t;                                 // 50 bytes total
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'N'
    char     stock[8];                         //  8 bytes | right-padded with spaces
    char     interest_flag;                    //  1 byte  | 'B' (buy), 'S' (sell), 'A' (both), 'N' (none)
} itch_rpii_t;                                 // 20 bytes total
#pragma pack(pop)

// DLCR messages
#pragma pack(push, 1)
typedef struct {
    itch_header_t header;                      // 11 bytes | message_type = 'O'
    char     stock[8];                         //  8 bytes | right-padded with spaces
    char     open_eligibility_status;          //  1 byte  | 'Y' or 'N'
    uint32_t minimum_allowable_price;          //  4 bytes | Price(4)
    uint32_t maximum_allowable_price;          //  4 bytes | Price(4)
    uint32_t near_execution_price;             //  4 bytes | Price(4)
    uint64_t near_execution_time;              //  8 bytes | nanoseconds since midnight
    uint32_t lower_price_range_collar;         //  4 bytes | Price(4)
    uint32_t upper_price_range_collar;         //  4 bytes | Price(4)
} itch_dlcr_t;                                 // 48 bytes total
#pragma pack(pop)

