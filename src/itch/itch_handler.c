#include <stdint.h>
#include <string.h>
#include "hft/itch/itch_handler.h"
#include "hft/itch/structs.h"
#include "hft/engine/events.h"
#include "hft/utils/utils.h"

#if defined(__SSE4_1__)
#include <smmintrin.h>  // _mm_shuffle_epi8 (PSHUFB) + _mm_extract_epi8/32
#define HFT_HAVE_PSHUFB 1
#endif

// PSHUFB byte-swap kernels for the order-lifecycle ITCH messages (A/F/E/X/C/D/U):
// one 16-byte shuffle replaces 3-5 scalar bswaps. Falls back when SSE4.1 absent.

#if defined(HFT_HAVE_PSHUFB)

// A/F: order_id @ msg+11 (8B BE), side @ msg+19, shares @ msg+20 (4B BE).
static inline void simd_unpack_af(const uint8_t *msg, event *out)
{
    static const int8_t kMask[16] = {
        10, 9, 8, 7, 6, 5, 4, 3,    // order_id LE  <- BE bytes 11..18
        15, 14, 13, 12,             // qty LE       <- BE bytes 20..23
        11,                         // side         <- byte 19
        -1, -1, -1
    };
    const __m128i mask = _mm_loadu_si128((const __m128i *)kMask);
    const __m128i v    = _mm_loadu_si128((const __m128i *)(msg + 8));
    const __m128i s    = _mm_shuffle_epi8(v, mask);
    _mm_storel_epi64((__m128i *)&out->order_id, s);
    out->qty  = (uint32_t)_mm_extract_epi32(s, 2);
    out->side = (uint8_t) _mm_extract_epi8 (s, 12);
}

// E/X: order_id @ msg+11 (8B BE), 4-byte int @ msg+19 (BE). No side field.
static inline void simd_unpack_ex(const uint8_t *msg, event *out)
{
    static const int8_t kMask[16] = {
        10, 9, 8, 7, 6, 5, 4, 3,    // order_id LE   <- BE bytes 11..18
        14, 13, 12, 11,             // qty (4B) LE   <- BE bytes 19..22
        -1, -1, -1, -1
    };
    const __m128i mask = _mm_loadu_si128((const __m128i *)kMask);
    const __m128i v    = _mm_loadu_si128((const __m128i *)(msg + 8));
    const __m128i s    = _mm_shuffle_epi8(v, mask);
    _mm_storel_epi64((__m128i *)&out->order_id, s);
    out->qty = (uint32_t)_mm_extract_epi32(s, 2);
}

// D: order_id @ msg+11 (8B BE) only.
static inline void simd_unpack_d(const uint8_t *msg, event *out)
{
    static const int8_t kMask[16] = {
        7, 6, 5, 4, 3, 2, 1, 0,
        -1, -1, -1, -1, -1, -1, -1, -1
    };
    const __m128i mask = _mm_loadu_si128((const __m128i *)kMask);
    const __m128i v    = _mm_loadu_si128((const __m128i *)(msg + 11));
    const __m128i s    = _mm_shuffle_epi8(v, mask);
    _mm_storel_epi64((__m128i *)&out->order_id, s);
}

// U: original_order_id @ msg+11 (8B), new_order_id @ msg+19 (8B), both swapped
// in one PSHUFB; shares/price live past the 16B window and are bswap'd scalarly.
static inline void simd_unpack_u(const uint8_t *msg, event *out)
{
    static const int8_t kMask[16] = {
        7, 6, 5, 4, 3, 2, 1, 0,     // original order_id
        15,14,13,12,11,10, 9, 8     // new order_id -> aux
    };
    const __m128i mask = _mm_loadu_si128((const __m128i *)kMask);
    const __m128i v    = _mm_loadu_si128((const __m128i *)(msg + 11));
    const __m128i s    = _mm_shuffle_epi8(v, mask);
    _mm_storeu_si128((__m128i *)&out->order_id, s);
}

#endif

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

// Administrative ITCH messages: dispatch table entry exists so the parser's
// null-check passes; they produce no event.
int handle_system_event           (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_stock_directory        (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_stock_trading_action   (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_reg_sho                (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_mkt_participant_pos    (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_mwcb_decline_level     (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_mwcb_status            (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_ipo_quoting_period_update(uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_luld_auction_collar    (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_operational_halt       (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_non_cross_trade        (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_cross_trade            (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_broken_trade           (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_noii                   (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_rpii                   (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }
int handle_dlcr                   (uint8_t* msg, event *out) { (void)msg; (void)out; return -1; }

// Order-lifecycle handlers (hot path)

int handle_add_order(uint8_t* msg, event *out) { // A
    out->aux          = 0;
    out->type         = msg[0];
    out->stock_locate = be16toh(*(const uint16_t *)(msg + 1));
    out->price        = be32toh(*(const uint32_t *)(msg + 32));
#if defined(HFT_HAVE_PSHUFB)
    simd_unpack_af(msg, out);
#else
    itch_add_order_t* ptr = (itch_add_order_t*) msg;
    out->order_id     = be64toh(ptr->order_reference_number);
    out->qty          = be32toh(ptr->shares);
    out->side         = ptr->buy_sell_indicator;
#endif
    return 0;
}

int handle_add_order_mpid(uint8_t* msg, event *out) { // F -- same layout as A
    out->aux          = 0;
    out->type         = msg[0];
    out->stock_locate = be16toh(*(const uint16_t *)(msg + 1));
    out->price        = be32toh(*(const uint32_t *)(msg + 32));
#if defined(HFT_HAVE_PSHUFB)
    simd_unpack_af(msg, out);
#else
    itch_add_order_mpid_t* ptr = (itch_add_order_mpid_t*) msg;
    out->order_id     = be64toh(ptr->order_reference_number);
    out->qty          = be32toh(ptr->shares);
    out->side         = ptr->buy_sell_indicator;
#endif
    return 0;
}

int handle_order_modify(uint8_t* msg, event *out) { // E
    out->aux          = 0;
    out->type         = msg[0];
    out->stock_locate = be16toh(*(const uint16_t *)(msg + 1));
    out->price        = 0;
    out->side         = 0;
#if defined(HFT_HAVE_PSHUFB)
    simd_unpack_ex(msg, out);
#else
    itch_order_modify_t* ptr = (itch_order_modify_t*) msg;
    out->order_id     = be64toh(ptr->order_reference_number);
    out->qty          = be32toh(ptr->executed_shares);
#endif
    return 0;
}

int handle_order_executed_price(uint8_t* msg, event *out) { // C
    out->aux          = 0;
    out->type         = msg[0];
    out->stock_locate = be16toh(*(const uint16_t *)(msg + 1));
    out->side         = 0;
    // order_id + executed_shares share the E/X window; price @ msg+32.
#if defined(HFT_HAVE_PSHUFB)
    simd_unpack_ex(msg, out);
#else
    itch_order_executed_price_t* ptr = (itch_order_executed_price_t*) msg;
    out->order_id     = be64toh(ptr->order_reference_number);
    out->qty          = be32toh(ptr->executed_shares);
#endif
    out->price        = be32toh(*(const uint32_t *)(msg + 32));
    return 0;
}

int handle_order_cancel(uint8_t* msg, event *out) { // X
    out->aux          = 0;
    out->type         = msg[0];
    out->stock_locate = be16toh(*(const uint16_t *)(msg + 1));
    out->price        = 0;
    out->side         = 0;
#if defined(HFT_HAVE_PSHUFB)
    simd_unpack_ex(msg, out);
#else
    itch_order_cancel_t* ptr = (itch_order_cancel_t*) msg;
    out->order_id     = be64toh(ptr->order_reference_number);
    out->qty          = be32toh(ptr->cancelled_shares);
#endif
    return 0;
}

int handle_order_delete(uint8_t* msg, event *out) { // D
    out->aux          = 0;
    out->type         = msg[0];
    out->stock_locate = be16toh(*(const uint16_t *)(msg + 1));
    out->qty          = 0;
    out->price        = 0;
    out->side         = 0;
#if defined(HFT_HAVE_PSHUFB)
    simd_unpack_d(msg, out);
#else
    itch_order_delete_t* ptr = (itch_order_delete_t*) msg;
    out->order_id     = be64toh(ptr->order_reference_number);
#endif
    return 0;
}

int handle_order_replace(uint8_t* msg, event *out) { // U
    // order_id = original ref (cancelled); aux = new ref. Side is recovered
    // downstream from the resting order on the old ref.
    out->type         = msg[0];
    out->stock_locate = be16toh(*(const uint16_t *)(msg + 1));
    out->side         = 0;
    out->qty          = be32toh(*(const uint32_t *)(msg + 27));
    out->price        = be32toh(*(const uint32_t *)(msg + 31));
#if defined(HFT_HAVE_PSHUFB)
    simd_unpack_u(msg, out);
#else
    itch_order_replace_t* ptr = (itch_order_replace_t*) msg;
    out->order_id     = be64toh(ptr->original_order_reference_number);
    out->aux          = be64toh(ptr->new_order_reference_number);
#endif
    return 0;
}
