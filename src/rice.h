/* SPDX-License-Identifier: Apache-2.0
 * Adaptive Rice code, sign mapping, adaptation, run mode. SPEC 5.2 to 5.5. */
#ifndef CRX_RICE_H
#define CRX_RICE_H
#include "bits.h"

extern const uint32_t crx_JS[32];
extern const uint8_t  crx_J[32];

static inline uint32_t crx_rice_code(crx_bits *b, unsigned k, unsigned escape_q, unsigned escape_bits)
{
    uint32_t q = crx_bits_zeros(b);
    if (q >= escape_q) return crx_bits_get(b, escape_bits);
    if (k == 0) return q;
    return (q << k) | crx_bits_get(b, k);
}
#define crx_code(b, k)    crx_rice_code((b), (k), 41, 21)
#define crx_code_qp(b, k) crx_rice_code((b), (k), 23, 8)

static inline int32_t crx_signed(uint32_t code)
{
    return (int32_t)(code >> 1) ^ -(int32_t)(code & 1);
}

/* kmax = 0 means no clamp. */
static inline unsigned crx_adapt(unsigned k, uint32_t c, unsigned kmax)
{
    unsigned nk = k - (c < ((1u << k) >> 1)) + ((c >> k) > 2) + ((c >> k) > 5);
    return (kmax && nk > kmax) ? kmax : nk;
}

/* Run length in run mode; `s` is the persistent run state. Returns the run
 * (0 when the first bit is 0) or UINT32_MAX when the stream is corrupt. */
static inline uint32_t crx_run(crx_bits *b, unsigned *s, uint32_t remaining)
{
    if (!crx_bits_get(b, 1)) return 0;
    uint32_t n = 1;
    while (crx_bits_get(b, 1)) {
        n += crx_JS[*s];
        if (n > remaining) { n = remaining; break; }
        if (*s < 31) (*s)++;
        if (n == remaining) break;
    }
    if (n < remaining) {
        if (crx_J[*s]) n += crx_bits_get(b, crx_J[*s]);
        if (*s > 0) (*s)--;
        if (n > remaining) return UINT32_MAX;
    }
    return n;
}
#endif
