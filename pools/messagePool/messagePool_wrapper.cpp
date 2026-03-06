#include "messagePool_wrapper.h"
#include "messagePool.hpp"

extern "C" {

static class MessagePool* g_pool = nullptr;

MessagePoolHandle MessagePool_create(int32_t nMessages) {
    g_pool = new class MessagePool(nMessages);
    return g_pool;
}

void MessagePool_destroy() {
    delete g_pool;
    g_pool = nullptr;
}

uint8_t* MessagePool_getMessage() {
    return g_pool->getMessage();
}

void MessagePool_removeMessage(const uint8_t* ptr) {
    g_pool->removeMessage(ptr);
}

uint8_t* MessagePool_getBase() {
    return g_pool->getBase();
}

}
