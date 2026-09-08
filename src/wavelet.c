/* SPDX-License-Identifier: Apache-2.0 */
#include "wavelet.h"
#include <stddef.h>
#include <string.h>

/* SPEC 8.1. h holds an extra at h[0] when `left` (the line's own h[0] is then h[1]).
 * Beyond the own counts, coefficients are seam extras when present in the arrays
 * (right) or mirrored. */
void crx_synth_line(const int32_t *l, uint32_t nl, const int32_t *h, uint32_t nh, bool left, bool right,
                    int32_t *x, uint32_t m)
{
    if (m == 0) return;
    if (m == 1) { x[0] = l[0]; return; }
    const int32_t *hh = h + (left ? 1 : 0);               /* hh[i] = own h[i]; hh[-1] valid when left */
    uint32_t nhh = nh - (left ? 1 : 0);                   /* own + right-extra count */
    uint32_t ne = (m + 1) / 2;                            /* even outputs */
    /* x[0] */
    x[0] = l[0] - (((left ? hh[-1] : hh[0]) + hh[0] + 2) >> 2);
    /* interior evens: i in 1 .. ne-1 need hh[i-1], hh[i]; hh[i] exists for i < nhh */
    uint32_t ilim = ne < nhh ? ne : nhh;                  /* evens whose hh[i] exists */
    for (uint32_t i = 1; i < ilim; i++)
        x[2 * i] = l[i] - ((hh[i - 1] + hh[i] + 2) >> 2);
    for (uint32_t i = ilim; i < ne; i++)                  /* only the last even when m odd and no right extra: mirror */
        x[2 * i] = l[i] - ((hh[i - 1] + hh[nhh - 1] + 2) >> 2);
    /* sample m, needed by the last odd when m is even */
    int32_t x_m = 0;
    if (!(m & 1)) {
        if (right && nl > m / 2 && nhh > m / 2) x_m = l[m / 2] - ((hh[m / 2 - 1] + hh[m / 2] + 2) >> 2);
        else x_m = x[m - 2];
    }
    uint32_t no = m / 2;                                  /* odd outputs */
    uint32_t olim = no;
    if (!(m & 1)) olim = no - 1;                          /* last odd uses x_m */
    for (uint32_t i = 0; i < olim; i++)
        x[2 * i + 1] = hh[i] + ((x[2 * i] + x[2 * i + 2]) >> 1);
    if (!(m & 1)) x[m - 1] = hh[no - 1] + ((x[m - 2] + x_m) >> 1);
}

size_t crx_stage_tmp_size(uint32_t m_w, uint32_t rows_l, uint32_t rows_h)
{
    return (size_t)m_w * (rows_l + rows_h + 1);
}

/* Vertical synthesis, row-oriented (SPEC 8.1 applied down columns, all
 * columns of a row at once). A: rows_l low rows, B: rows_h high rows (B[0]
 * is the seam extra when `top`), both m_w wide; out: m_h rows. */
static void vertical(const int32_t *A, uint32_t rows_l, const int32_t *B, uint32_t rows_h, bool top, bool bottom,
                     int32_t *out, uint32_t m_w, uint32_t m_h, size_t so, int32_t *xm)
{
    if (m_h == 1) { memcpy(out, A, m_w * sizeof *A); return; }
    const int32_t *Bh = B + (top ? (size_t)m_w : 0);      /* own high rows */
    uint32_t nbh = rows_h - (top ? 1 : 0);
    uint32_t ne = (m_h + 1) / 2, no = m_h / 2;
    /* even rows */
    {
        const int32_t *hp = top ? B : Bh;                 /* row "-1" */
        const int32_t *hc = Bh, *lc = A; int32_t *o = out;
        for (uint32_t c = 0; c < m_w; c++) o[c] = lc[c] - ((hp[c] + hc[c] + 2) >> 2);
    }
    uint32_t ilim = ne < nbh ? ne : nbh;
    for (uint32_t i = 1; i < ilim; i++) {
        const int32_t *hp = Bh + (size_t)(i - 1) * m_w, *hc = hp + m_w, *lc = A + (size_t)i * m_w;
        int32_t *o = out + (size_t)(2 * i) * so;
        for (uint32_t c = 0; c < m_w; c++) o[c] = lc[c] - ((hp[c] + hc[c] + 2) >> 2);
    }
    for (uint32_t i = ilim; i < ne; i++) {
        const int32_t *hp = Bh + (size_t)(i - 1) * m_w, *hc = Bh + (size_t)(nbh - 1) * m_w, *lc = A + (size_t)i * m_w;
        int32_t *o = out + (size_t)(2 * i) * so;
        for (uint32_t c = 0; c < m_w; c++) o[c] = lc[c] - ((hp[c] + hc[c] + 2) >> 2);
    }
    /* row m_h (beyond), when m_h is even */
    const int32_t *beyond = NULL;
    if (!(m_h & 1)) {
        if (bottom && rows_l > m_h / 2 && nbh > m_h / 2) {
            const int32_t *hp = Bh + (size_t)(m_h / 2 - 1) * m_w, *hc = hp + m_w, *lc = A + (size_t)(m_h / 2) * m_w;
            for (uint32_t c = 0; c < m_w; c++) xm[c] = lc[c] - ((hp[c] + hc[c] + 2) >> 2);
            beyond = xm;
        } else beyond = out + (size_t)(m_h - 2) * so;
    }
    /* odd rows */
    uint32_t olim = (m_h & 1) ? no : no - 1;
    for (uint32_t i = 0; i < olim; i++) {
        const int32_t *hc = Bh + (size_t)i * m_w, *ea = out + (size_t)(2 * i) * so, *eb = ea + 2 * so;
        int32_t *o = out + (size_t)(2 * i + 1) * so;
        for (uint32_t c = 0; c < m_w; c++) o[c] = hc[c] + ((ea[c] + eb[c]) >> 1);
    }
    if (!(m_h & 1)) {
        const int32_t *hc = Bh + (size_t)(no - 1) * m_w, *ea = out + (size_t)(m_h - 2) * so;
        int32_t *o = out + (size_t)(m_h - 1) * so;
        for (uint32_t c = 0; c < m_w; c++) o[c] = hc[c] + ((ea[c] + beyond[c]) >> 1);
    }
}

void crx_synth_stage(const int32_t *ll, uint32_t wl, uint32_t hl, size_t sll,
                     const int32_t *hlb, uint32_t whl, uint32_t hhl, size_t shl,
                     const int32_t *lhb, uint32_t wlh, uint32_t hlh, size_t slh,
                     const int32_t *hhb, uint32_t whh, uint32_t hhh, size_t shh,
                     bool left, bool right, bool top, bool bottom,
                     int32_t *out, uint32_t m_w, uint32_t m_h, size_t so, int32_t *tmp)
{
    (void)hhl; (void)hhh;
    uint32_t rows_l = hl, rows_h = hlh;
    int32_t *A = tmp, *B = tmp + (size_t)m_w * rows_l, *xm = B + (size_t)m_w * rows_h;
    for (uint32_t r = 0; r < rows_l; r++)
        crx_synth_line(ll + r * sll, wl, hlb + r * shl, whl, left, right, A + (size_t)r * m_w, m_w);
    for (uint32_t r = 0; r < rows_h; r++)
        crx_synth_line(lhb + r * slh, wlh, hhb + r * shh, whh, left, right, B + (size_t)r * m_w, m_w);
    vertical(A, rows_l, B, rows_h, top, bottom, out, m_w, m_h, so, xm);
}
