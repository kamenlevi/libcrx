/* SPDX-License-Identifier: Apache-2.0
 * An encoder written from SPEC 5, used only to make test streams. It mirrors
 * the decoder's state machine so a round trip proves the two agree; the
 * corpus proves the decoder agrees with Canon. */
#ifndef CRX_ENC_H
#define CRX_ENC_H
#include "../src/rice.h"
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *p; size_t n, cap; uint64_t acc; unsigned nbits; } ebits;

static void eb_bit(ebits *e, unsigned bit)
{
    e->acc = (e->acc << 1) | (bit & 1); e->nbits++;
    if (e->nbits == 8) {
        if (e->n == e->cap) { e->cap = e->cap * 2 + 64; e->p = realloc(e->p, e->cap); }
        e->p[e->n++] = (uint8_t)e->acc; e->acc = 0; e->nbits = 0;
    }
}
static void eb_bits(ebits *e, uint32_t v, unsigned k) { while (k--) eb_bit(e, (v >> k) & 1); }
static void eb_flush(ebits *e) { while (e->nbits) eb_bit(e, 0); }

static uint32_t zigzag(int32_t v) { return v >= 0 ? (uint32_t)v << 1 : ((uint32_t)(-(v + 1)) << 1) | 1; }

/* Rice code with escape (SPEC 5.2). */
static void enc_code(ebits *e, uint32_t code, unsigned k, unsigned escape_q, unsigned escape_bits)
{
    uint32_t q = code >> k;
    if (q >= escape_q) { for (unsigned i = 0; i < escape_q; i++) eb_bit(e, 0); eb_bit(e, 1); eb_bits(e, code, escape_bits); return; }
    for (uint32_t i = 0; i < q; i++) eb_bit(e, 0);
    eb_bit(e, 1);
    eb_bits(e, code & ((1u << k) - 1), k);
}

/* Run of `t` (0 <= t <= remaining) with state s (SPEC 5.5). */
static void enc_run(ebits *e, unsigned *s, uint32_t t, uint32_t remaining)
{
    if (t == 0) { eb_bit(e, 0); return; }
    eb_bit(e, 1);
    uint32_t n = 1;
    for (;;) {
        if (n == remaining) return;                    /* decoder stopped by itself */
        if (n + crx_JS[*s] <= t) {
            eb_bit(e, 1); n += crx_JS[*s];
            if (*s < 31) (*s)++;
            if (n == remaining) return;
        } else { eb_bit(e, 0); break; }
    }
    if (crx_J[*s]) eb_bits(e, t - n, crx_J[*s]);
    if (*s > 0) (*s)--;
}

typedef struct { ebits e; unsigned k, s; uint32_t width; int32_t *prev, *cur, *kp; uint32_t line; } enc_state;

static inline void enc_init(enc_state *st, uint32_t width)
{
    memset(st, 0, sizeof *st); st->width = width;
    st->prev = (int32_t *)calloc(width + 2, 4) + 1; st->cur = (int32_t *)calloc(width + 2, 4) + 1; st->kp = (int32_t *)calloc(width + 2, 4) + 1;
}

static inline void enc_free(enc_state *st)
{
    free(st->prev - 1); free(st->cur - 1); free(st->kp - 1); free(st->e.p);
}

static inline int32_t enc_med(int32_t a, int32_t b, int32_t c)
{
    int32_t mx = a > b ? a : b, mn = a < b ? a : b;
    if (c >= mx) return mn;
    if (c <= mn) return mx;
    return a + b - c;
}
static inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

static inline void enc_sym_ll(enc_state *st, int32_t value, int32_t pred, uint32_t x, int more)
{
    uint32_t v = zigzag(value - pred);
    enc_code(&st->e, v, st->k, 41, 21);
    if (more) v = (v + (uint32_t)iabs(2 * (st->prev[x + 1] - st->prev[x]))) >> 1;
    st->k = crx_adapt(st->k, v, 15);
}

/* Encode one base-band line (SPEC 5.6); `vals` has width entries. */
static inline void enc_line_ll(enc_state *st, const int32_t *vals)
{
    uint32_t w = st->width; int32_t *c = st->cur, *p = st->prev;
    if (st->line == 0) {
        c[-1] = 0; uint32_t x = 0;
        while (x < w) {
            if (x < w - 1 && c[(int32_t)x - 1] == 0) {
                uint32_t t = 0; while (x + t < w && vals[x + t] == 0) t++;
                enc_run(&st->e, &st->s, t, w - x);
                for (uint32_t i = 0; i < t; i++) c[x + i] = 0;
                x += t; if (x == w) break;
            }
            enc_sym_ll(st, vals[x], c[(int32_t)x - 1], x, 0); c[x] = vals[x]; x++;
        }
    } else {
        c[-1] = p[0]; uint32_t x = 0;
        while (x < w) {
            int32_t a = c[(int32_t)x - 1], b = p[x], cc = p[(int32_t)x - 1], d = p[x + 1];
            if (x == w - 1) { enc_sym_ll(st, vals[x], enc_med(a, b, cc), x, 0); c[x] = vals[x]; break; }
            if (!(a == b && a == d)) { enc_sym_ll(st, vals[x], enc_med(a, b, cc), x, 1); c[x] = vals[x]; x++; }
            else {
                uint32_t t = 0; while (x + t < w && vals[x + t] == a) t++;
                enc_run(&st->e, &st->s, t, w - x);
                for (uint32_t i = 0; i < t; i++) c[x + i] = a;
                x += t;
                if (x < w) { enc_sym_ll(st, vals[x], p[x], x, x < w - 1); c[x] = vals[x]; x++; }
            }
        }
    }
    c[w] = c[w - 1] + 1;
    int32_t *t = st->prev; st->prev = st->cur; st->cur = t; st->line++;
}

static inline void enc_sym_hf(enc_state *st, int32_t value, uint32_t x, int shifted, int last)
{
    uint32_t v = zigzag(value); if (shifted) v -= 1;
    enc_code(&st->e, v, st->k, 41, 21);
    if (last) { st->k = crx_adapt(st->k, v, 15); }
    else {
        unsigned k = crx_adapt(st->k, v, 0);
        if (st->kp[x + 1] - (int32_t)k <= 1) { if (k > 15) k = 15; } else k++;
        st->k = k;
    }
    st->kp[x] = (int32_t)st->k;
}

/* Encode one high-pass line (SPEC 5.7). */
static inline void enc_line_hf(enc_state *st, const int32_t *vals)
{
    uint32_t w = st->width; int32_t *c = st->cur, *p = st->prev;
    c[-1] = 0; uint32_t x = 0;
    while (x < w - 1) {
        if (p[x + 1] | p[x] | c[(int32_t)x - 1]) { enc_sym_hf(st, vals[x], x, 0, 0); c[x] = vals[x]; x++; }
        else {
            uint32_t t = 0; while (x + t < w && vals[x + t] == 0) t++;
            enc_run(&st->e, &st->s, t, w - x);
            for (uint32_t i = 0; i < t; i++) { c[x + i] = 0; st->kp[x + i] = 0; }
            x += t;
            if (x == w - 1) { enc_sym_hf(st, vals[x], x, 1, 1); c[x] = vals[x]; x++; }
            else if (x < w - 1) { enc_sym_hf(st, vals[x], x, 1, 0); c[x] = vals[x]; x++; }
        }
    }
    if (x == w - 1) { enc_sym_hf(st, vals[x], x, 0, 1); c[x] = vals[x]; }
    int32_t *t = st->prev; st->prev = st->cur; st->cur = t; st->line++;
}
#endif
