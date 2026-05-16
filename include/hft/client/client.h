#pragma once

#include "hft/server/server.h"
#include "hft/utils/moldudp64.h"
#include "hft/ring_buffers/parser/parser_to_engine.h"
#include "hft/ring_buffers/order/order_to_exc.h"

#define SERVER_IP BIND_IP

void *udp_receiver_thread(void *arg);
void *order_sender_thread(void *arg);
