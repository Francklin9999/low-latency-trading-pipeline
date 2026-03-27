#pragma once

#include "hft/utils/moldudp64.h"
#include "hft/ring_buffers/parser/parser_to_engine.h"
#include "hft/ring_buffers/order/order_to_exc.h"

#define SERVER_IP "127.0.0.1"
#define FEED_PORT 5000 // UDP feed
#define ORDER_PORT 5001 // TCP outgoing

void *udp_receiver_thread(void *arg);
void *order_sender_thread(void *arg);