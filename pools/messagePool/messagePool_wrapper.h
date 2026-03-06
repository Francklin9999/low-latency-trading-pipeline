#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* MessagePoolHandle;

MessagePoolHandle MessagePool_create(int32_t nMessages);
void  MessagePool_destroy();

uint8_t* MessagePool_getMessage();
void MessagePool_removeMessage(const uint8_t*);
uint8_t* MessagePool_getBase();

#ifdef __cplusplus
}
#endif
