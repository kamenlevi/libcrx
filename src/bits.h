/* SPDX-License-Identifier: Apache-2.0
 * Big-endian bit reader over one subband's bytes. SPEC 5.1. Reads past the
 * end yield zero bits and are counted, never faulted. */
#ifndef CRX_BITS_H
#define CRX_BITS_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct crx_bits {
    const uint8_t *p, *end;
    uint64_t acc;          /* valid bits are the top `n` bits */
    unsigned n;
    uint32_t overrun;      /* bytes supplied past the end */
} crx_bits;

static inline void crx_bits_init(crx_bits *b, const uint8_t *p, size_t len)
{
    b->p = p; b->end = p + len; b->acc = 0; b->n = 0; b->overrun = 0;
}

static inline void crx_bits_refill(crx_bits *b)
{
#ifndef CRX_NO_REFILL32
    if (b->n <= 32 && b->end - b->p >= 4) {
        /* the common case: four bytes at once, big-endian */
        uint32_t w = (uint32_t)b->p[0] << 24 | (uint32_t)b->p[1] << 16 | (uint32_t)b->p[2] << 8 | b->p[3];
        b->p += 4;
        b->acc |= (uint64_t)w << (32 - b->n);
        b->n += 32;
    }
#endif
    while (b->n <= 56) {
        uint64_t byte = 0;
        if (b->p < b->end) byte = *b->p++; else b->overrun++;
        b->acc |= byte << (56 - b->n);
        b->n += 8;
    }
}

/* Next `k` bits (0 <= k <= 32) as an unsigned integer. */
static inline uint32_t crx_bits_get(crx_bits *b, unsigned k)
{
    if (k == 0) return 0;
    if (b->n < k) crx_bits_refill(b);
    uint32_t v = (uint32_t)(b->acc >> (64 - k));
    b->acc <<= k; b->n -= k;
    return v;
}

/* Count zero bits up to and including the next one bit. Past the end the
 * count is capped so a hostile stream cannot spin. */
static inline uint32_t crx_bits_zeros(crx_bits *b)
{
    uint32_t total = 0;
    for (;;) {
        if (b->n < 32) crx_bits_refill(b);
        if (b->acc) {
            unsigned z = (unsigned)__builtin_clzll(b->acc);
            b->acc = (z + 1 < 64) ? b->acc << (z + 1) : 0;    /* a shift by 64 is undefined */
            b->n -= z + 1;
            return total + z;
        }
        total += b->n; b->n = 0;
        if (b->overrun > 16) return total;          /* nothing but padding left */
    }
}

/* Bits consumed beyond the band's data, for the harness (SPEC 5.1). */
static inline uint32_t crx_bits_overrun_bits(const crx_bits *b)
{
    uint32_t supplied = b->overrun * 8;
    return supplied > b->n ? supplied - b->n : 0;
}
#endif
