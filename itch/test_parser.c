#include "../itch/itch_handler.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "structs.h"

int main() {
    const char* filepath = "../data/01302020.NASDAQ_ITCH50";

    int fd = open(filepath, O_RDONLY);
    if (fd == -1) {
        perror("Error opening file");
    }

    
    ssize_t n;
    char buffer[64000];
    uint16_t message_size;
    // first 20 bytes should be the header
    int offset = 20;
    int leftover = 0;
    while((n = read(fd, buffer + leftover, sizeof(buffer) - leftover)) > 0) {
        n += leftover;
        offset = 0;
        while (1) {
            if (offset + 2 > n)
                break;
            message_size = be16toh(*(uint16_t*) &buffer[offset]);
            if (offset + 2 + message_size > n)
                break;
            char messsage_type = (char) buffer[offset + 2];
            itch_handler_fn fn = dispatch_table[(unsigned char)messsage_type];
            if (fn == NULL) { offset += message_size + 2; continue; }
            void* result = (*fn)((uint8_t*) &buffer[offset + 2]);
            if (messsage_type == 'A') {
                itch_add_order_t* o = (itch_add_order_t*) result;
                printf("ADD  ref=%-20llu side=%c shares=%-6u stock=%.8s price=$%.2f    message_type=%c\n",
                    (unsigned long long)o->order_reference_number,
                    o->buy_sell_indicator,
                    o->shares,
                    o->stock,
                    o->price / 10000.0,
                    o->header.message_type);
            }
            offset += message_size + 2;
        }
        leftover = n - offset;
        memmove(buffer, &buffer[offset], leftover);
    }

    return 0;
}