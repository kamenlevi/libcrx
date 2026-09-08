/* SPDX-License-Identifier: Apache-2.0
 * Line decoders for the base band (SPEC 5.6) and the high-pass bands (5.7). */
#ifndef CRX_LINES_H
#define CRX_LINES_H
#include "rice.h"

typedef struct crx_linestate {
    crx_bits bits;
    unsigned k, s;
    uint32_t width;
    uint32_t line;        /* lines decoded so far */
    int32_t *buf0, *buf1; /* two lines of width + 2, index -1..width; buf[0] is pixel 0 */
    int32_t *kp;          /* high-pass: per-column k memory, width + 2, index -1..width */
    bool corrupt;
} crx_linestate;

/* `mem` must hold 3 * (width + 2) int32. */
void crx_line_init(crx_linestate *st, const uint8_t *data, size_t len, uint32_t width, int32_t *mem);
/* Decode the next line into st's current buffer; returns a pointer to pixel 0
 * of that line (valid until the next call), or NULL when corrupt. */
const int32_t *crx_line_ll(crx_linestate *st);
const int32_t *crx_line_hf(crx_linestate *st);
#endif
