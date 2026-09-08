/* SPDX-License-Identifier: Apache-2.0 */
#include "wavelet.h"
#include <stddef.h>

static inline int32_t H_at(const int32_t *h, uint32_t nh, bool left, bool right, int32_t i)
{
    /* i is an index in the line's own numbering; the array holds an extra at the front when left. */
    int32_t base = left ? 1 : 0;
    int32_t avail = (int32_t)nh - base;                 /* own + right extras */
    if (i < 0) return left ? h[0] : h[base];            /* seam extra or mirror h[-1] = h[0] */
    if (i >= avail) return h[base + avail - 1];         /* only reached without right extras: mirror */
    (void)right;
    return h[base + i];
}

void crx_synth_line(const int32_t *l, uint32_t nl, const int32_t *h, uint32_t nh, bool left, bool right,
                    int32_t *x, uint32_t m)
{
    if (m == 0) return;
    if (m == 1) { x[0] = l[0]; return; }
    uint32_t ne = (m + 1) / 2;                          /* even samples 0, 2, ..., 2*(ne-1) */
    for (uint32_t i = 0; i < ne; i++)
        x[2 * i] = l[i] - ((H_at(h, nh, left, right, (int32_t)i - 1) + H_at(h, nh, left, right, (int32_t)i) + 2) >> 2);
    int32_t x_m;                                        /* sample m (beyond the line), needed when m is even */
    if (m & 1) x_m = 0;
    else if (right && nl > m / 2) x_m = l[m / 2] - ((H_at(h, nh, left, right, (int32_t)m / 2 - 1) + H_at(h, nh, left, right, (int32_t)m / 2) + 2) >> 2);
    else x_m = x[m - 2];
    for (uint32_t i = 0; 2 * i + 1 < m; i++) {
        int32_t next = (2 * i + 2 < m) ? x[2 * i + 2] : x_m;
        x[2 * i + 1] = H_at(h, nh, left, right, (int32_t)i) + ((x[2 * i] + next) >> 1);
    }
}

size_t crx_stage_tmp_size(uint32_t m_w, uint32_t rows_l, uint32_t rows_h)
{
    return (size_t)m_w * (rows_l + rows_h) + 4 * (size_t)(rows_l + rows_h + 2);
}

void crx_synth_stage(const int32_t *ll, uint32_t wl, uint32_t hl, size_t sll,
                     const int32_t *hlb, uint32_t whl, uint32_t hhl, size_t shl,
                     const int32_t *lhb, uint32_t wlh, uint32_t hlh, size_t slh,
                     const int32_t *hhb, uint32_t whh, uint32_t hhh, size_t shh,
                     bool left, bool right, bool top, bool bottom,
                     int32_t *out, uint32_t m_w, uint32_t m_h, size_t so, int32_t *tmp)
{
    (void)hlh; (void)hhh;
    /* Rows first: A[r] from (LL row r, HL row r), r < hl; B[r] from (LH row r, HH row r), r < hlh. */
    uint32_t rows_l = hl, rows_h = hlh;
    int32_t *A = tmp, *B = tmp + (size_t)m_w * rows_l;
    for (uint32_t r = 0; r < rows_l; r++)
        crx_synth_line(ll + r * sll, wl, hlb + r * shl, whl, left, right, A + (size_t)r * m_w, m_w);
    for (uint32_t r = 0; r < rows_h; r++)
        crx_synth_line(lhb + r * slh, wlh, hhb + r * shh, whh, left, right, B + (size_t)r * m_w, m_w);
    (void)hhl;
    /* Columns: for each column, a line of rows_l low and rows_h high samples -> m_h outputs. */
    int32_t *cl = B + (size_t)m_w * rows_h, *ch = cl + rows_l + 2, *cx = ch + rows_h + 2;
    for (uint32_t c = 0; c < m_w; c++) {
        for (uint32_t r = 0; r < rows_l; r++) cl[r] = A[(size_t)r * m_w + c];
        for (uint32_t r = 0; r < rows_h; r++) ch[r] = B[(size_t)r * m_w + c];
        crx_synth_line(cl, rows_l, ch, rows_h, top, bottom, cx, m_h);
        for (uint32_t r = 0; r < m_h; r++) out[(size_t)r * so + c] = cx[r];
    }
}
