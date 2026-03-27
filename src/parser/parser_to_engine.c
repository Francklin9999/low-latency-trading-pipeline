#include "hft/parser/parser_to_engine.h"
#include "hft/itch/itch_handler.h"
#include "hft/ring_buffers/event/event_to_engine.h"
#include "hft/ring_buffers/parser/parser_to_engine.h"
#include "hft/utils/utils.h"

packet_ref* read_from_parser(void) {
    for (;;) {
        uint32_t read = atomic_load_explicit(&PARSER_ENGINE.next_read, memory_order_relaxed);
        uint32_t write = atomic_load_explicit(&PARSER_ENGINE.next_write, memory_order_acquire);

        if (read != write) {
            packet_ref* ptr = PARSER_ENGINE.data[read & PARSER_ENGINE_MASK];
            return ptr;
        }
    }
}

void parse_to_engine(void) {
    packet_ref* ref = read_from_parser();
    uint32_t offset = 0;

    while (offset + 2u <= ref->len) {
        uint16_t message_size = be16toh(*(const uint16_t*)(ref->data + offset));
        uint32_t next_offset = offset + 2u + (uint32_t)message_size;

        if (next_offset > ref->len) {
            break;
        }

        unsigned char message_type = ref->data[offset + 2u];
        itch_handler_fn fn = dispatch_table[message_type];
        if (fn != NULL) {
            uint32_t write = atomic_load_explicit(&EVENT_ENGINE->next_write, memory_order_relaxed);

            while ((uint32_t)(write - atomic_load_explicit(&EVENT_ENGINE->next_read, memory_order_acquire)) ==
                   EVENT_TO_ENGINE_SIZE) {
                write = atomic_load_explicit(&EVENT_ENGINE->next_write, memory_order_relaxed);
            }

            event* slot = &EVENT_ENGINE->data[write & EVENT_ENGINE_MASK];
            int ok = fn((uint8_t*)(ref->data + offset + 2u), slot);

            if (ok == 0) {
                atomic_store_explicit(&EVENT_ENGINE->next_write, write + 1u, memory_order_release);
            }
        }

        offset = next_offset;
    }

    atomic_fetch_add_explicit(&PARSER_ENGINE.next_read, 1u, memory_order_release);
}
