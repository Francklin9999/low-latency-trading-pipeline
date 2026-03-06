#pragma once
#include <stdint.h>
#include <time.h>

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#ifndef be16toh
#define be16toh(x) __builtin_bswap16(x)
#endif
#ifndef be64toh
#define be64toh(x) __builtin_bswap64(x)
#endif
#else
#ifndef be16toh
#define be16toh(x) (x)
#endif
#ifndef be64toh
#define be64toh(x) (x)
#endif
#endif

static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#endif
}

static inline uint64_t local_ns(void) {
    struct timespec _ts;
    clock_gettime(CLOCK_MONOTONIC, &_ts);
    return (uint64_t)_ts.tv_sec * 1000000000ULL + (uint64_t)_ts.tv_nsec;
}