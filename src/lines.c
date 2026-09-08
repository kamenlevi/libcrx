/* SPDX-License-Identifier: Apache-2.0 */
#include "lines.h"
#include <string.h>
#include <stdlib.h>

void crx_line_init(crx_linestate *st, const uint8_t *data, size_t len, uint32_t width, int32_t *mem)
{
    crx_bits_init(&st->bits, data, len);
    st->k = 0; st->s = 0; st->width = width; st->line = 0; st->corrupt = false;
    st->buf0 = mem + 1; st->buf1 = mem + (width + 2) + 1; st->kp = mem + 2 * (width + 2) + 1;
    memset(mem, 0, 3 * (width + 2) * sizeof *mem);
}

static inline int32_t med(int32_t a, int32_t b, int32_t c)
{
    int32_t t = b - c;
    unsigned i = ((unsigned)((c < a) ^ (t < 0)) << 1) | (unsigned)((a < b) ^ (t < 0));
    switch (i) { case 0: case 1: return a + t; case 2: return a; default: return b; }
}

static inline int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

/* SPEC 5.6 symbol: prediction `pred`, gradient blend when `more`. */
static inline int32_t sym_ll(crx_linestate *st, int32_t pred, const int32_t *p, uint32_t x, bool more)
{
    uint32_t v = crx_code(&st->bits, st->k);
    int32_t out = pred + crx_signed(v);
    if (more) v = (v + (uint32_t)abs32(2 * (p[x + 1] - p[x]))) >> 1;
    st->k = crx_adapt(st->k, v, 15);
    return out;
}

static bool line_ll(crx_linestate *st, const int32_t *p, int32_t *c)
{
    uint32_t w = st->width;
    if (st->line == 0) {
        c[-1] = 0;
        uint32_t x = 0;
        while (x < w) {
            if (x < w - 1 && c[(int32_t)x - 1] == 0) {        /* the last pixel never enters run mode */
                uint32_t n = crx_run(&st->bits, &st->s, w - x);
                if (n == UINT32_MAX) { st->corrupt = true; return false; }
                for (uint32_t i = 0; i < n; i++) c[x + i] = 0;
                x += n;
                if (x == w) break;
            }
            uint32_t v = crx_code(&st->bits, st->k);
            c[x] = c[(int32_t)x - 1] + crx_signed(v);
            st->k = crx_adapt(st->k, v, 15);
            x++;
        }
    } else {
        c[-1] = p[0];
        uint32_t x = 0;
        while (x < w) {
            int32_t a = c[(int32_t)x - 1], b = p[x], cc = p[(int32_t)x - 1], d = p[x + 1];
            if (x == w - 1) { c[x] = sym_ll(st, med(a, b, cc), p, x, false); break; }
            if (!(a == b && a == d)) {
                c[x] = sym_ll(st, med(a, b, cc), p, x, true); x++;
            } else {
                uint32_t n = crx_run(&st->bits, &st->s, w - x);
                if (n == UINT32_MAX) { st->corrupt = true; return false; }
                for (uint32_t i = 0; i < n; i++) c[x + i] = a;
                x += n;
                if (x < w) { c[x] = sym_ll(st, p[x], p, x, x < w - 1); x++; }
            }
        }
    }
    c[w] = c[w - 1] + 1;
    st->line++;
    return true;
}

const int32_t *crx_line_ll(crx_linestate *st)
{
    if (st->corrupt) return NULL;
    int32_t *p = (st->line & 1) ? st->buf1 : st->buf0, *c = (st->line & 1) ? st->buf0 : st->buf1;
    return line_ll(st, p, c) ? c : NULL;
}
bool crx_line_ll_into(crx_linestate *st, const int32_t *prev, int32_t *cur)
{
    return !st->corrupt && line_ll(st, prev, cur);
}

/* SPEC 5.7 symbol with the per-column k memory. */
static inline void sym_hf(crx_linestate *st, int32_t *c, uint32_t x, bool shifted)
{
    uint32_t v = crx_code(&st->bits, st->k);
    c[x] = crx_signed(shifted ? v + 1 : v);
    unsigned k = crx_adapt(st->k, v, 0);
    if ((int32_t)st->kp[x + 1] - (int32_t)k <= 1) { if (k > 15) k = 15; }
    else k++;
    st->k = k; st->kp[x] = (int32_t)k;
}

static inline void sym_hf_last(crx_linestate *st, int32_t *c, uint32_t x, bool shifted)
{
    uint32_t v = crx_code(&st->bits, st->k);
    c[x] = crx_signed(shifted ? v + 1 : v);
    st->k = crx_adapt(st->k, v, 15);
    st->kp[x] = (int32_t)st->k;
}

static bool line_hf(crx_linestate *st, const int32_t *p, int32_t *c)
{
    uint32_t w = st->width;
    c[-1] = 0;
    uint32_t x = 0;
    while (x < w - 1) {
        if (p[x + 1] | p[x] | c[(int32_t)x - 1]) {
            sym_hf(st, c, x, false); x++;
        } else {
            uint32_t n = crx_run(&st->bits, &st->s, w - x);
            if (n == UINT32_MAX) { st->corrupt = true; return false; }
            for (uint32_t i = 0; i < n; i++) { c[x + i] = 0; st->kp[x + i] = 0; }
            x += n;
            if (x == w - 1) { sym_hf_last(st, c, x, true); x++; }
            else if (x < w - 1) { sym_hf(st, c, x, true); x++; }
        }
    }
    if (x == w - 1) sym_hf_last(st, c, x, false);
    st->line++;
    return true;
}

const int32_t *crx_line_hf(crx_linestate *st)
{
    if (st->corrupt) return NULL;
    int32_t *p = (st->line & 1) ? st->buf1 : st->buf0, *c = (st->line & 1) ? st->buf0 : st->buf1;
    return line_hf(st, p, c) ? c : NULL;
}
bool crx_line_hf_into(crx_linestate *st, const int32_t *prev, int32_t *cur)
{
    return !st->corrupt && line_hf(st, prev, cur);
}
