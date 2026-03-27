#include "hft/pools/eventPool/eventPool_wrapper.h"
#include "hft/pools/eventPool/eventPool.hpp"

extern "C" {

static class EventPool* g_pool = nullptr;

EventPoolHandle EventPool_create(int32_t nEvents) {
    g_pool = new class EventPool(nEvents);
    return g_pool;
}

void EventPool_destroy() {
    delete g_pool;
    g_pool = nullptr;
}

int32_t EventPool_availableCount() {
    return g_pool->availableCount();
}

event* EventPool_getEvent() {
    return g_pool->getEvent();
}

void EventPool_removeEvent(const event* ptr) noexcept {
    g_pool->removeEvent(ptr);
}

}
