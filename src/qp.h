/* SPDX-License-Identifier: Apache-2.0 — SPEC 7: quantisation. */
#ifndef CRX_QP_H
#define CRX_QP_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* step(q) of SPEC 7.1; returns 0 for q >= 36 (out of scope). */
uint32_t crx_qstep(int32_t q);

typedef struct crx_qmap {
    uint32_t qw, qh;             /* map size: ceil(w/8) x ceil(h/2) */
    uint32_t *S[3];              /* S[0] level 1 (qh rows), S[1] level 2 (ceil(h/4)), S[2] level 3 (ceil(h/8)) */
    uint32_t rows[3];
} crx_qmap;

/* Decode a tile's QP table (bytes `data`, `len`) for a tile plane w x h with
 * `levels` wavelet levels into `qm`, using `mem` of crx_qmap_mem_size ints.
 * Returns false when corrupt (negative average). `overrun` receives bits read past the data. */
size_t crx_qmap_mem_size(uint32_t w, uint32_t h);
bool crx_qmap_decode(const uint8_t *data, size_t len, uint32_t w, uint32_t h, unsigned levels, crx_qmap *qm, uint32_t *mem, uint32_t *overrun);
#endif
