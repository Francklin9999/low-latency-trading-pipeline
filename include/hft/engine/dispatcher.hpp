#pragma once

#include "hft/engine/batch_structs.hpp"

#include "hft/ring_buffers/order/order_to_exc.h"

#define NUMBER_OF_DISPATCHERS 2

#define NUMBER_OF_PING_PONG_BUFFERS 2

struct BatchBuffer {
    AddOrderBatch addOrder[NUMBER_OF_DISPATCHERS];
    AddOrderMpidBatch addOrderMpid[NUMBER_OF_DISPATCHERS];
    OrderModifyBatch orderModify[NUMBER_OF_DISPATCHERS];
    OrderExecutedPriceBatch orderExecPrice[NUMBER_OF_DISPATCHERS];
    OrderCancelBatch orderCancel[NUMBER_OF_DISPATCHERS];
    OrderDeleteBatch orderDelete[NUMBER_OF_DISPATCHERS];
    OrderReplaceBatch orderReplace[NUMBER_OF_DISPATCHERS];
};

extern BatchBuffer g_buf[NUMBER_OF_PING_PONG_BUFFERS];
extern int g_ping;