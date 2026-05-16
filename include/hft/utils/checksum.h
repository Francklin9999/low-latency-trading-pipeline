#pragma once

#include <stdint.h>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

static inline uint32_t cksum_partial_simd(const void *data, uint32_t len,
                                           uint32_t seed)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = seed;

#if defined(__AVX2__)
    if (len >= 32) {
        __m256i acc = _mm256_setzero_si256();
        const __m256i lo16 = _mm256_set1_epi32(0x0000FFFF);
        while (len >= 32) {
            const __m256i v  = _mm256_loadu_si256((const __m256i *)p);
            const __m256i lo = _mm256_and_si256(v, lo16);
            const __m256i hi = _mm256_srli_epi32(v, 16);
            acc = _mm256_add_epi32(acc, lo);
            acc = _mm256_add_epi32(acc, hi);
            p   += 32;
            len -= 32;
        }
        __m128i hi128 = _mm256_extracti128_si256(acc, 1);
        __m128i lo128 = _mm256_castsi256_si128(acc);
        __m128i s     = _mm_add_epi32(lo128, hi128);
        s = _mm_add_epi32(s, _mm_srli_si128(s, 8));
        s = _mm_add_epi32(s, _mm_srli_si128(s, 4));
        sum += (uint32_t)_mm_cvtsi128_si32(s);
    }
#endif

    while (len >= 2) {
        sum += (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        p   += 2;
        len -= 2;
    }
    if (len == 1) sum += (uint32_t)p[0];
    return sum;
}

static inline uint16_t cksum_fold16(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)sum;
}
