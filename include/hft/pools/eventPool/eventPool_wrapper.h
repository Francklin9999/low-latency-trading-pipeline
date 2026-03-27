#pragma once

#include <stdint.h>
#include "hft/engine/events.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* EventPoolHandle;

EventPoolHandle EventPool_create(int32_t nEvents);
void EventPool_destroy();

int32_t EventPool_availableCount();

event* EventPool_getEvent();
void EventPool_removeEvent(const event*) noexcept;

#ifdef __cplusplus
}
#endif
