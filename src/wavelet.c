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
                     int32_t *out, uint32_t m_w, uint32_t m_h, size_t so, int32_t *xm, uint32_t c0, uint32_t c1)
{
    if (m_h == 1) { memcpy(out + c0, A + c0, (c1 - c0) * sizeof *A); return; }
    const int32_t *Bh = B + (top ? (size_t)m_w : 0);      /* own high rows */
    uint32_t nbh = rows_h - (top ? 1 : 0);
    uint32_t ne = (m_h + 1) / 2, no = m_h / 2;
    /* even rows */
    {
        const int32_t *hp = top ? B : Bh;                 /* row "-1" */
        const int32_t *hc = Bh, *lc = A; int32_t *o = out;
        for (uint32_t c = c0; c < c1; c++) o[c] = lc[c] - ((hp[c] + hc[c] + 2) >> 2);
    }
    uint32_t ilim = ne < nbh ? ne : nbh;
    for (uint32_t i = 1; i < ilim; i++) {
        const int32_t *hp = Bh + (size_t)(i - 1) * m_w, *hc = hp + m_w, *lc = A + (size_t)i * m_w;
        int32_t *o = out + (size_t)(2 * i) * so;
        for (uint32_t c = c0; c < c1; c++) o[c] = lc[c] - ((hp[c] + hc[c] + 2) >> 2);
    }
    for (uint32_t i = ilim; i < ne; i++) {
        const int32_t *hp = Bh + (size_t)(i - 1) * m_w, *hc = Bh + (size_t)(nbh - 1) * m_w, *lc = A + (size_t)i * m_w;
        int32_t *o = out + (size_t)(2 * i) * so;
        for (uint32_t c = c0; c < c1; c++) o[c] = lc[c] - ((hp[c] + hc[c] + 2) >> 2);
    }
    /* row m_h (beyond), when m_h is even */
    const int32_t *beyond = NULL;
    if (!(m_h & 1)) {
        if (bottom && rows_l > m_h / 2 && nbh > m_h / 2) {
            const int32_t *hp = Bh + (size_t)(m_h / 2 - 1) * m_w, *hc = hp + m_w, *lc = A + (size_t)(m_h / 2) * m_w;
            for (uint32_t c = c0; c < c1; c++) xm[c] = lc[c] - ((hp[c] + hc[c] + 2) >> 2);
            beyond = xm;
        } else beyond = out + (size_t)(m_h - 2) * so;
    }
    /* odd rows */
    uint32_t olim = (m_h & 1) ? no : no - 1;
    for (uint32_t i = 0; i < olim; i++) {
        const int32_t *hc = Bh + (size_t)i * m_w, *ea = out + (size_t)(2 * i) * so, *eb = ea + 2 * so;
        int32_t *o = out + (size_t)(2 * i + 1) * so;
        for (uint32_t c = c0; c < c1; c++) o[c] = hc[c] + ((ea[c] + eb[c]) >> 1);
    }
    if (!(m_h & 1)) {
        const int32_t *hc = Bh + (size_t)(no - 1) * m_w, *ea = out + (size_t)(m_h - 2) * so;
        int32_t *o = out + (size_t)(m_h - 1) * so;
        for (uint32_t c = c0; c < c1; c++) o[c] = hc[c] + ((ea[c] + beyond[c]) >> 1);
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
    vertical(A, rows_l, B, rows_h, top, bottom, out, m_w, m_h, so, xm, 0, m_w);
}

void crx_stage_rows(const crx_stage *s, uint32_t r0, uint32_t r1)
{
    int32_t *A = s->tmp, *B = s->tmp + (size_t)s->m_w * s->hl;
    for (uint32_t r = r0; r < r1; r++) {
        if (r < s->hl) crx_synth_line(s->ll + r * s->sll, s->wl, s->hlb + r * s->shl, s->whl, s->left, s->right, A + (size_t)r * s->m_w, s->m_w);
        else { uint32_t q = r - s->hl; crx_synth_line(s->lhb + q * s->slh, s->wlh, s->hhb + q * s->shh, s->whh, s->left, s->right, B + (size_t)q * s->m_w, s->m_w); }
    }
}

void crx_stage_cols(const crx_stage *s, uint32_t c0, uint32_t c1)
{
    int32_t *A = s->tmp, *B = s->tmp + (size_t)s->m_w * s->hl, *xm = B + (size_t)s->m_w * s->hlh;
    vertical(A, s->hl, B, s->hlh, s->top, s->bottom, s->out, s->m_w, s->m_h, s->so, xm, c0, c1);
}

/* ---- strip synthesis ------------------------------------------------------
 * Vertical 5/3 on rows [y0, y1) needs low rows A[i] for the even outputs and
 * one beyond, high rows B[i-1..i+1]. Each needed A/B row is one horizontal
 * synthesis, computed here into scratch. Boundary rules as in vertical(). */
size_t crx_strip_scratch_size(uint32_t m_w, uint32_t strip_rows)
{
    /* A rows: strip_rows/2 + 3, B rows: strip_rows/2 + 3, plus two output rows */
    return (size_t)m_w * (strip_rows + 10);
}

void crx_stage_strip(const crx_stage *s, uint32_t y0, uint32_t y1, int32_t *scratch, crx_row_sink emit, void *ctx)
{
    uint32_t m_w = s->m_w, m_h = s->m_h;
    if (m_h == 1) {
        int32_t *row = scratch;
        crx_synth_line(s->ll, s->wl, s->hlb, s->whl, s->left, s->right, row, m_w);
        if (emit) emit(ctx, 0, row); else memcpy(s->out, row, m_w * sizeof *row);
        return;
    }
    uint32_t rows_l = s->hl, rows_h = s->hlh;
    uint32_t nbh = rows_h - (s->top ? 1 : 0);
    /* index ranges: even outputs 2i for i in [ia0, ia1]; odd outputs 2i+1 need x[2i+2] -> even i+1 */
    int32_t i_first = (int32_t)(y0 / 2), i_last = (int32_t)((y1 - 1) / 2);
    int32_t ia1 = i_last + 1;                                  /* one even beyond for the last odd */
    /* high rows needed: B[i-1 .. i] for evens, plus B[i] for odds; in own numbering (top extra is B row -1) */
    int32_t ib0 = i_first - 1, ib1 = ia1;
    /* clamp to what exists; mirrored/extra rows are handled by the accessors below */
    int32_t nA = ia1 - i_first + 1, nB = ib1 - ib0 + 1;
    int32_t *A = scratch, *B = A + (size_t)nA * m_w, *E = B + (size_t)nB * m_w;   /* E: two even rows (prev, cur) */
    /* horizontal passes */
    for (int32_t i = i_first; i <= ia1; i++) {
        int32_t *dst = A + (size_t)(i - i_first) * m_w;
        if ((uint32_t)i < rows_l) crx_synth_line(s->ll + (size_t)i * s->sll, s->wl, s->hlb + (size_t)i * s->shl, s->whl, s->left, s->right, dst, m_w);
    }
    for (int32_t j = ib0; j <= ib1; j++) {
        int32_t *dst = B + (size_t)(j - ib0) * m_w;
        int32_t r = j + (s->top ? 1 : 0);                      /* row in the LH/HH arrays */
        if (j < 0 && !s->top) continue;                        /* mirror: handled by accessor */
        if (r >= 0 && (uint32_t)r < rows_h) crx_synth_line(s->lhb + (size_t)r * s->slh, s->wlh, s->hhb + (size_t)r * s->shh, s->whh, s->left, s->right, dst, m_w);
    }
    #define AROW(i) (A + (size_t)((i) - i_first) * m_w)
    /* high row accessor with mirror at both ends (own numbering j; j = -1 valid when top) */
    #define BROWJ(j) (B + (size_t)((j) - ib0) * m_w)
    /* even row 2i = A[i] - ((H(i-1) + H(i) + 2) >> 2), H(-1) = top ? extra : H(0); H(j >= nbh) = H(nbh-1) unless bottom extra exists */
    int32_t *even_prev = E, *even_cur = E + m_w;
    /* We need even rows for i in [i_first, ia1] but only those < m_h are output; x[m_h] (beyond) when m_h even and needed */
    int32_t have_prev = 0;
    for (int32_t i = i_first; i <= ia1; i++) {
        uint32_t y = 2 * (uint32_t)i;
        const int32_t *hp, *hc;
        int32_t jp = i - 1, jc = i;
        if (jp < 0) hp = s->top ? BROWJ(-1) : BROWJ(0);
        else hp = BROWJ(jp < (int32_t)nbh ? jp : (int32_t)nbh - 1);
        hc = BROWJ(jc < (int32_t)nbh ? jc : (int32_t)nbh - 1);
        int32_t *ev = even_cur;
        if (y < m_h || (y == m_h && !(m_h & 1) && s->bottom && (uint32_t)i < rows_l && (uint32_t)i < nbh)) {
            const int32_t *lc = AROW(i);
            for (uint32_t c = 0; c < m_w; c++) ev[c] = lc[c] - ((hp[c] + hc[c] + 2) >> 2);
        } else if (y == m_h && !(m_h & 1)) {
            memcpy(ev, even_prev, m_w * sizeof *ev);          /* mirror: x[m_h] = x[m_h - 2] */
        } else break;
        if (y < m_h && y >= y0 && y < y1) { if (emit) emit(ctx, y, ev); else memcpy(s->out + (size_t)y * s->so, ev, m_w * sizeof *ev); }
        /* odd row 2i-1 = B[i-1] + ((x[2i-2] + x[2i]) >> 1), available once ev (x[2i]) exists */
        if (have_prev) {
            uint32_t yo = y - 1;
            if (yo >= y0 && yo < y1 && yo < m_h) {
                const int32_t *hb = BROWJ(i - 1);
                int32_t *od = AROW(i - 1);                     /* reuse the consumed A row as output scratch */
                for (uint32_t c = 0; c < m_w; c++) od[c] = hb[c] + ((even_prev[c] + ev[c]) >> 1);
                if (emit) emit(ctx, yo, od); else memcpy(s->out + (size_t)yo * s->so, od, m_w * sizeof *od);
            }
        }
        int32_t *t = even_prev; even_prev = even_cur; even_cur = t; have_prev = 1;
        (void)hc;
    }
    #undef AROW
    #undef BROWJ
}
