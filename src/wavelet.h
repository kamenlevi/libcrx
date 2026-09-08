/* SPDX-License-Identifier: Apache-2.0 — SPEC 8: integer 5/3 synthesis. */
#ifndef CRX_WAVELET_H
#define CRX_WAVELET_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* One line: low-pass l[0..nl), high-pass h[0..nh) where, when `left`, h[0]
 * is the seam extra (belonging before the line) and the line's own first
 * high-pass coefficient is h[1]. Produces x[0..m). Coefficients beyond the
 * own counts are seam extras when `right`, otherwise mirrored. */
void crx_synth_line(const int32_t *l, uint32_t nl, const int32_t *h, uint32_t nh, bool left, bool right,
                    int32_t *x, uint32_t m);

/* One stage: LL (wl x hl, row stride sl), HL (whl x hhl), LH (wlh x hlh), HH (whh x hhh),
 * flags for seam extras, output out (m_w x m_h, stride so). `tmp` holds
 * 2 * m_w * max(hl, hlh) + ... see wavelet.c; use crx_stage_tmp_size. */
size_t crx_stage_tmp_size(uint32_t m_w, uint32_t rows_l, uint32_t rows_h);
void crx_synth_stage(const int32_t *ll, uint32_t wl, uint32_t hl, size_t sll,
                     const int32_t *hlb, uint32_t whl, uint32_t hhl, size_t shl,
                     const int32_t *lhb, uint32_t wlh, uint32_t hlh, size_t slh,
                     const int32_t *hhb, uint32_t whh, uint32_t hhh, size_t shh,
                     bool left, bool right, bool top, bool bottom,
                     int32_t *out, uint32_t m_w, uint32_t m_h, size_t so, int32_t *tmp);

/* Split form of crx_synth_stage for parallel execution: the same inputs,
 * horizontal pass over a range of A/B rows (index space: rows_l A rows then
 * rows_h B rows), then vertical pass over a column range. tmp as before. */
typedef struct crx_stage {
    const int32_t *ll; uint32_t wl, hl; size_t sll;
    const int32_t *hlb; uint32_t whl, hhl; size_t shl;
    const int32_t *lhb; uint32_t wlh, hlh; size_t slh;
    const int32_t *hhb; uint32_t whh, hhh; size_t shh;
    bool left, right, top, bottom;
    int32_t *out; uint32_t m_w, m_h; size_t so;
    int32_t *tmp;
} crx_stage;
void crx_stage_rows(const crx_stage *s, uint32_t r0, uint32_t r1);   /* r in [0, hl + hlh) */
void crx_stage_cols(const crx_stage *s, uint32_t c0, uint32_t c1);   /* c in [0, m_w) */

/* Strip form: compute output rows [y0, y1) alone, recomputing the few
 * horizontal rows they need into `scratch` (crx_strip_scratch_size ints).
 * Writes either int32 rows to s->out (when emit is NULL) or calls emit for
 * each finished row with the row's int32 samples. */
typedef void (*crx_row_sink)(void *ctx, uint32_t y, const int32_t *row);
size_t crx_strip_scratch_size(uint32_t m_w, uint32_t strip_rows);
void crx_stage_strip(const crx_stage *s, uint32_t y0, uint32_t y1, int32_t *scratch, crx_row_sink emit, void *ctx);
#endif
