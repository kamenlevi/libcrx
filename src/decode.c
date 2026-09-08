/* SPDX-License-Identifier: Apache-2.0
 * Decoding proper: lossless planes (SPEC 5, 9) and lossy planes (SPEC 7, 8, 10). */
#include "crx_internal.h"
#include "lines.h"
#include "qp.h"
#include "wavelet.h"
#include "pool.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static int trace_on = -1;
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }
#define TRACE_INIT() do { if (trace_on < 0) trace_on = getenv("CRX_TRACE") != NULL; } while (0)

/* Row and column offset inside the 2x2 cell for plane p under each CFA layout (SPEC 9). */
static const uint8_t cell_row[4][4] = { {0,0,1,1}, {0,0,1,1}, {1,1,0,0}, {1,1,0,0} };
static const uint8_t cell_col[4][4] = { {0,1,0,1}, {1,0,1,0}, {0,1,0,1}, {1,0,1,0} };

/* Write a w x h block of plane samples (row stride ss) into the mosaic at plane position (px, py). */
static void emit_plane(const crx_decoder *d, uint32_t pi, const int32_t *src, size_t ss, uint32_t w, uint32_t h,
                       uint32_t px, uint32_t py, uint16_t *dst, size_t stride)
{
    const int32_t median = 1 << (d->bits - 1), maxv = (1 << d->bits) - 1;
    uint32_t r0 = cell_row[d->cfa][pi], c0 = cell_col[d->cfa][pi];
    for (uint32_t y = 0; y < h; y++) {
        const int32_t *line = src + (size_t)y * ss;
        uint16_t *row = dst + (size_t)(2 * (py + y) + r0) * stride + 2 * px + c0;
        for (uint32_t x = 0; x < w; x++) {
            int32_t v = median + line[x];
            row[2 * x] = (uint16_t)(v < 0 ? 0 : v > maxv ? maxv : v);
        }
    }
}

static crx_status decode_lossless_plane(crx_decoder *d, const crx_tile *t, uint32_t pi, uint16_t *dst, size_t stride)
{
    const crx_plane *pl = &t->planes[pi];
    const crx_band *b = &pl->bands[0];
    if (b->data_off + b->data_size > d->len) return CRX_E_TRUNCATED;
    crx_linestate st;
    crx_line_init(&st, d->bytes + b->data_off, b->data_size, t->w, d->scratch + (size_t)pi * d->scratch_per_plane);
    for (uint32_t y = 0; y < t->h; y++) {
        const int32_t *line = pl->partial ? crx_line_ll(&st) : crx_line_hf(&st);
        if (!line) return CRX_E_CORRUPT;
        emit_plane(d, pi, line, 0, t->w, 1, t->x0, t->y0 + y, dst, stride);
    }
    d->overrun_bits += crx_bits_overrun_bits(&st.bits);
    return CRX_OK;
}

/* Padded band buffer: (height + 1) rows of (width + 2), row -1 zero, pixel 0 at column 1. */
#define BSTRIDE(b) ((size_t)(b)->width + 2)
#define BSIZE(b)   (BSTRIDE(b) * ((size_t)(b)->height + 1))
#define BROW(buf, b, y) ((buf) + BSTRIDE(b) * ((size_t)(y) + 1) + 1)

/* SPEC 7.2 / 7.3: multiply one band line by its steps. `lvl` is 1 (finest) .. N. */
static inline void dequant_line(const crx_decoder *d, const crx_band *b, unsigned lvl, const crx_qmap *qm, uint32_t y, int32_t *line)
{
    if (d->version == 0x100) {
        uint32_t m = crx_qstep((int32_t)b->q);
        if (m == 1) return;
        for (uint32_t x = 0; x < b->width; x++) line[x] = (int32_t)((uint32_t)line[x] * m);
        return;
    }
    const uint32_t *S = qm->S[lvl - 1]; uint32_t rows = qm->rows[lvl - 1], qw = qm->qw;
    unsigned sh = 3 - lvl;
    uint32_t own_w = b->width - b->left_extra - b->right_extra, own_h = b->height - b->top_extra - b->bottom_extra;
    uint32_t ry = y < b->top_extra ? 0 : (y - b->top_extra < own_h ? y - b->top_extra : own_h - 1);
    if (ry >= rows) ry = rows - 1;
    const uint32_t *srow = S + (size_t)ry * qw;
    for (uint32_t x = 0; x < b->width; x++) {
        uint32_t rx = x < b->left_extra ? 0 : ((x - b->left_extra < own_w ? x - b->left_extra : own_w - 1) >> sh);
        if (rx >= qw) rx = qw - 1;
        int32_t m = (int32_t)b->qbase + (int32_t)((srow[rx] * b->qmult) >> 3);
        if (m < 1) m = 1; else if (m > 0x168000) m = 0x168000;
        line[x] = (int32_t)((uint32_t)line[x] * (uint32_t)m);
    }
}

/* Decode one whole band into a padded buffer, dequantising each line while it is hot.
 * `lvl` 0 means no dequantisation (lossless). */
static crx_status decode_band(crx_decoder *d, const crx_band *b, bool base, unsigned lvl, const crx_qmap *qm, int32_t *buf, int32_t *mem)
{
    if (b->data_off + b->data_size > d->len) return CRX_E_TRUNCATED;
    memset(buf, 0, BSTRIDE(b) * sizeof *buf);                      /* row -1 */
    if (b->data_size == 0) { memset(buf, 0, BSIZE(b) * sizeof *buf); return CRX_OK; }
    if (lvl && d->version == 0x100 && b->q >= 36) return CRX_E_UNSUPPORTED;
    if (lvl && d->version == 0x200 && !qm) return CRX_E_UNSUPPORTED;
    crx_linestate st;
    crx_line_init(&st, d->bytes + b->data_off, b->data_size, b->width, mem);
    for (uint32_t y = 0; y < b->height; y++) {
        int32_t *cur = BROW(buf, b, y); const int32_t *prev = cur - BSTRIDE(b);
        if (!(base ? crx_line_ll_into(&st, prev, cur) : crx_line_hf_into(&st, prev, cur))) return CRX_E_CORRUPT;
        if (lvl) dequant_line(d, b, lvl, qm, y, cur);
    }
    d->overrun_bits += crx_bits_overrun_bits(&st.bits);
    return CRX_OK;
}

static crx_status decode_lossy_plane(crx_decoder *d, const crx_tile *t, uint32_t pi, const crx_qmap *qm,
                                     unsigned level, uint32_t px_level, uint32_t py_level, uint16_t *dst, size_t stride,
                                     int32_t *raw, size_t raw_stride, int32_t **ext, uint32_t *ew, uint32_t *eh)
{
    const crx_plane *pl = &t->planes[pi];
    unsigned N = d->levels;
    /* Workspace: all bands, N intermediate outputs, line memory, stage temp. */
    size_t need = 0;
    for (unsigned k = 0; k < d->nbands; k++) need += BSIZE(&pl->bands[k]);
    size_t out_w[CRX_MAX_LEVELS + 1], out_h[CRX_MAX_LEVELS + 1];    /* output of the stage at level L, index L */
    for (unsigned L = 1; L <= N; L++) {
        if (L == 1) {
            out_w[L] = t->w + ((ext && (t->flags & CRX_RIGHT) && !(t->w & 1)) ? 1 : 0);
            out_h[L] = t->h + ((ext && (t->flags & CRX_BOTTOM) && !(t->h & 1)) ? 1 : 0);
        }
        else { const crx_band *hl = &pl->bands[3 * (N - (L - 1)) + 1]; out_w[L] = hl[1].width; out_h[L] = hl[0].height; }
        need += out_w[L] * out_h[L];
    }
    uint32_t maxw = 0, maxrl = 0, maxrh = 0;
    for (unsigned k = 0; k < d->nbands; k++) if (pl->bands[k].width > maxw) maxw = pl->bands[k].width;
    for (unsigned L = 1; L <= N; L++) {
        const crx_band *hl = &pl->bands[3 * (N - L) + 1];
        if (hl[0].height > maxrl) maxrl = hl[0].height;
        if (hl[1].height > maxrh) maxrh = hl[1].height;
        if (out_w[L] > maxw) maxw = (uint32_t)out_w[L];
    }
    size_t tmp_sz = crx_stage_tmp_size(maxw, maxrl, maxrh);
    need += 3 * ((size_t)maxw + 2) + tmp_sz;
    int32_t *ws = malloc(need * sizeof *ws);
    if (!ws) return CRX_E_NOMEM;
    int32_t *bands[CRX_MAX_BANDS], *outs[CRX_MAX_LEVELS + 1], *p = ws;
    for (unsigned k = 0; k < d->nbands; k++) { bands[k] = p; p += BSIZE(&pl->bands[k]); }
    for (unsigned L = 1; L <= N; L++) { outs[L] = p; p += out_w[L] * out_h[L]; }
    int32_t *linemem = p; p += 3 * ((size_t)maxw + 2);
    int32_t *tmp = p;

    crx_status s = CRX_OK;
    TRACE_INIT();
    double t0 = trace_on ? now_ms() : 0, tdec = 0;
    for (unsigned k = 0; k < d->nbands && s == CRX_OK; k++) {
        const crx_band *b = &pl->bands[k];
        unsigned lvl = k == 0 ? N : N - (k - 1) / 3;              /* band index -> wavelet level */
        if (lvl <= level) continue;                                /* not needed for this output level */
        double ta = trace_on ? now_ms() : 0;
        s = decode_band(d, b, k == 0 && pl->partial, lvl, qm, bands[k], linemem);
        if (trace_on) { double tb = now_ms(); tdec += tb - ta; fprintf(stderr, "  band %u %ux%u decode+dequant %.2f ms (%.0f Msym/s)\n", k, b->width, b->height, tb - ta, (double)b->width * b->height / 1e3 / (tb - ta)); }
    }
    if (trace_on) fprintf(stderr, " plane %u tile %u: bands %.1f ms\n", pi, (unsigned)(t - d->tiles), tdec);
    /* Stages from the coarsest level N down to level `level` + 1. */
    const int32_t *ll = BROW(bands[0], &pl->bands[0], 0); uint32_t wl = pl->bands[0].width, hl = pl->bands[0].height; size_t sll = BSTRIDE(&pl->bands[0]);
    for (unsigned L = N; L > level && s == CRX_OK; L--) {
        const crx_band *hb = &pl->bands[3 * (N - L) + 1];
        crx_synth_stage(ll, wl, hl, sll,
                        BROW(bands[3 * (N - L) + 1], &hb[0], 0), hb[0].width, hb[0].height, BSTRIDE(&hb[0]),
                        BROW(bands[3 * (N - L) + 2], &hb[1], 0), hb[1].width, hb[1].height, BSTRIDE(&hb[1]),
                        BROW(bands[3 * (N - L) + 3], &hb[2], 0), hb[2].width, hb[2].height, BSTRIDE(&hb[2]),
                        t->flags & CRX_LEFT, t->flags & CRX_RIGHT, t->flags & CRX_TOP, t->flags & CRX_BOTTOM,
                        outs[L], (uint32_t)out_w[L], (uint32_t)out_h[L], out_w[L], tmp);
        ll = outs[L]; wl = (uint32_t)out_w[L]; hl = (uint32_t)out_h[L]; sll = out_w[L];
        if (trace_on) { double t1 = now_ms(); fprintf(stderr, "  stage %u -> %ux%u: %.2f ms\n", L, wl, hl, t1 - t0); t0 = t1; }
    }
    if (trace_on) t0 = now_ms();
    if (s == CRX_OK) {
        uint32_t ow = crx_ceil2n(t->w, level), oh = crx_ceil2n(t->h, level);
        if (dst) emit_plane(d, pi, ll, sll, ow, oh, px_level, py_level, dst, stride);
        if (raw) for (uint32_t y = 0; y < oh; y++) memcpy(raw + (size_t)y * raw_stride, ll + (size_t)y * sll, ow * sizeof *raw);
        if (trace_on) fprintf(stderr, "  emit: %.2f ms\n", now_ms() - t0);
        if (ext) {
            *ext = malloc((size_t)wl * hl * sizeof **ext);
            if (*ext) { for (uint32_t y = 0; y < hl; y++) memcpy(*ext + (size_t)y * wl, ll + (size_t)y * sll, wl * sizeof **ext); *ew = wl; *eh = hl; } else s = CRX_E_NOMEM;
        }
    }
    free(ws);
    return s;
}

static crx_status tile_qmap(crx_decoder *d, const crx_tile *t, crx_qmap *qm, uint32_t **qmem);

/* ---- Parallel lossy pipeline -------------------------------------------------
 * Phase 1: every needed (tile, plane, band) decodes independently.
 * Phase 2: per level, horizontal pass tasks (tile, plane, row block), then
 *          vertical pass tasks (tile, plane, column block).
 * Phase 3: emit tasks (tile, plane, row block). */

typedef struct pp_plane {
    const crx_tile *t; uint32_t ti, pi; const crx_qmap *qm;
    int32_t *bands[CRX_MAX_BANDS]; int32_t *linemem[CRX_MAX_BANDS];
    int32_t *outs[CRX_MAX_LEVELS + 1]; size_t out_w[CRX_MAX_LEVELS + 1], out_h[CRX_MAX_LEVELS + 1];
    int32_t *tmp; size_t tmp_sz;
    crx_stage st;                       /* current stage */
    const int32_t *final; uint32_t final_w, final_h; size_t final_s;
    uint32_t px, py;                    /* plane position at the output level */
    atomic_int status;
    uint64_t overrun;
} pp_plane;

typedef struct pp_ctx {
    crx_decoder *d; unsigned level; uint16_t *dst; size_t stride;
    pp_plane *pl; uint32_t nplanes;
    /* task tables */
    uint32_t *band_task;                /* phase 1: (plane index << 4) | band */
    uint32_t nband_tasks;
    uint32_t rows_per, cols_per;
    uint32_t *blk_plane, *blk_start, *blk_end; uint32_t nblk;    /* phase 2/3 blocks */
} pp_ctx;

static void task_band(void *vctx, uint32_t i)
{
    pp_ctx *c = vctx; uint32_t code = c->band_task[i]; pp_plane *P = &c->pl[code >> 4]; unsigned k = code & 15;
    const crx_plane *pl = &P->t->planes[P->pi]; const crx_band *b = &pl->bands[k];
    unsigned N = c->d->levels, lvl = k == 0 ? N : N - (k - 1) / 3;
    crx_decoder tmpd = *c->d; tmpd.overrun_bits = 0;           /* private overrun accumulator */
    crx_status s = decode_band(&tmpd, b, k == 0 && pl->partial, lvl, P->qm, P->bands[k], P->linemem[k]);
    __atomic_fetch_add(&P->overrun, tmpd.overrun_bits, __ATOMIC_RELAXED);
    if (s != CRX_OK) atomic_store(&P->status, (int)s);
}

static void task_rows(void *vctx, uint32_t i) { pp_ctx *c = vctx; pp_plane *P = &c->pl[c->blk_plane[i]]; crx_stage_rows(&P->st, c->blk_start[i], c->blk_end[i]); }
static void task_cols(void *vctx, uint32_t i) { pp_ctx *c = vctx; pp_plane *P = &c->pl[c->blk_plane[i]]; crx_stage_cols(&P->st, c->blk_start[i], c->blk_end[i]); }
static void task_emit(void *vctx, uint32_t i)
{
    pp_ctx *c = vctx; pp_plane *P = &c->pl[c->blk_plane[i]];
    uint32_t y0 = c->blk_start[i], y1 = c->blk_end[i];
    emit_plane(c->d, P->pi, P->final + (size_t)y0 * P->final_s, P->final_s, P->final_w, y1 - y0, P->px, P->py + y0, c->dst, c->stride);
}

/* Build blocks of [start, end) over `total` per plane, `per` each. */
static uint32_t make_blocks(pp_ctx *c, uint32_t (*total)(const pp_plane *, void *), void *arg, uint32_t per)
{
    uint32_t n = 0;
    for (uint32_t p = 0; p < c->nplanes; p++) {
        uint32_t tot = total(&c->pl[p], arg);
        for (uint32_t s0 = 0; s0 < tot; s0 += per) {
            c->blk_plane[n] = p; c->blk_start[n] = s0; c->blk_end[n] = s0 + per < tot ? s0 + per : tot; n++;
        }
    }
    return c->nblk = n;
}
static uint32_t tot_rows(const pp_plane *P, void *arg) { (void)arg; return P->st.hl + P->st.hlh; }
static uint32_t tot_cols(const pp_plane *P, void *arg) { (void)arg; return P->st.m_w; }
static uint32_t tot_emit(const pp_plane *P, void *arg) { (void)arg; return P->final_h; }

static crx_status decode_lossy_all(crx_decoder *d, unsigned level, uint16_t *dst, size_t stride)
{
    unsigned N = d->levels;
    uint32_t ntiles = d->tiles_x * d->tiles_y, nplanes = ntiles * d->nplanes;
    pp_ctx c = { d, level, dst, stride, NULL, nplanes, NULL, 0, 64, 512, NULL, NULL, NULL, 0 };
    c.pl = calloc(nplanes, sizeof *c.pl);
    if (!c.pl) return CRX_E_NOMEM;
    crx_status s = CRX_OK;
    TRACE_INIT();
    double T0 = trace_on ? now_ms() : 0;
    /* QP maps per tile */
    crx_qmap *qms = calloc(ntiles, sizeof *qms); uint32_t **qmem = calloc(ntiles, sizeof *qmem);
    if (!qms || !qmem) { s = CRX_E_NOMEM; goto out_small; }
    for (uint32_t ti = 0; ti < ntiles && s == CRX_OK; ti++) s = tile_qmap(d, &d->tiles[ti], &qms[ti], &qmem[ti]);
    if (s != CRX_OK) goto out_small;
    /* per-plane workspaces */
    size_t max_blocks = 0;
    uint32_t px = 0, py = 0;
    for (uint32_t ti = 0; ti < ntiles; ti++) {
        const crx_tile *t = &d->tiles[ti];
        uint32_t tx = ti % d->tiles_x, ty = ti / d->tiles_x;
        px = 0; for (uint32_t k = 0; k < tx; k++) px += crx_ceil2n(d->tiles[ty * d->tiles_x + k].w, level);
        py = 0; for (uint32_t k = 0; k < ty; k++) py += crx_ceil2n(d->tiles[k * d->tiles_x].h, level);
        for (uint32_t pi = 0; pi < d->nplanes; pi++) {
            pp_plane *P = &c.pl[ti * d->nplanes + pi];
            P->t = t; P->ti = ti; P->pi = pi; P->qm = qmem[ti] ? &qms[ti] : NULL; P->px = px; P->py = py;
            const crx_plane *pl = &t->planes[pi];
            size_t need = 0;
            for (unsigned k = 0; k < d->nbands; k++) need += BSIZE(&pl->bands[k]) + 3 * ((size_t)pl->bands[k].width + 2);
            uint32_t maxw = 0, maxrl = 0, maxrh = 0;
            for (unsigned L = 1; L <= N; L++) {
                if (L == 1) { P->out_w[L] = t->w; P->out_h[L] = t->h; }
                else { const crx_band *hl = &pl->bands[3 * (N - (L - 1)) + 1]; P->out_w[L] = hl[1].width; P->out_h[L] = hl[0].height; }
                if (L > level) need += P->out_w[L] * P->out_h[L];
                const crx_band *hl = &pl->bands[3 * (N - L) + 1];
                if (hl[0].height > maxrl) maxrl = hl[0].height;
                if (hl[1].height > maxrh) maxrh = hl[1].height;
                if (P->out_w[L] > maxw) maxw = (uint32_t)P->out_w[L];
            }
            P->tmp_sz = crx_stage_tmp_size(maxw, maxrl, maxrh); need += P->tmp_sz;
            int32_t *ws = crx_ws_get(need * sizeof *ws), *p = ws;
            if (!ws) { s = CRX_E_NOMEM; break; }
            P->bands[0] = ws;                                     /* keep the base pointer for free() */
            for (unsigned k = 0; k < d->nbands; k++) { P->bands[k] = p; p += BSIZE(&pl->bands[k]); P->linemem[k] = p; p += 3 * ((size_t)pl->bands[k].width + 2); }
            for (unsigned L = 1; L <= N; L++) if (L > level) { P->outs[L] = p; p += P->out_w[L] * P->out_h[L]; }
            P->tmp = p;
            size_t blocks = (t->h + maxrl + maxrh) / c.rows_per + (maxw / c.cols_per) + 4;
            if (blocks > max_blocks) max_blocks = blocks;
        }
        if (s != CRX_OK) break;
    }
    if (s != CRX_OK) goto out;
    c.band_task = malloc((size_t)nplanes * d->nbands * sizeof *c.band_task);
    c.blk_plane = malloc(max_blocks * nplanes * sizeof *c.blk_plane);
    c.blk_start = malloc(max_blocks * nplanes * sizeof *c.blk_start);
    c.blk_end = malloc(max_blocks * nplanes * sizeof *c.blk_end);
    if (!c.band_task || !c.blk_plane || !c.blk_start || !c.blk_end) { s = CRX_E_NOMEM; goto out; }
    /* Phase 1: bands, largest first so the long ones start early. */
    c.nband_tasks = 0;
    for (unsigned k = d->nbands; k-- > 0;) {
        unsigned lvl = k == 0 ? N : N - (k - 1) / 3;
        if (lvl <= level) continue;
        for (uint32_t p = 0; p < nplanes; p++) c.band_task[c.nband_tasks++] = (p << 4) | k;
    }
    if (trace_on) { double T1 = now_ms(); fprintf(stderr, "setup+qmaps %.2f ms\n", T1 - T0); T0 = T1; }
    crx_pool_run(d->pool, c.nband_tasks, task_band, &c);
    if (trace_on) { double T1 = now_ms(); fprintf(stderr, "phase 1 bands (%u tasks) %.2f ms\n", c.nband_tasks, T1 - T0); T0 = T1; }
    for (uint32_t p = 0; p < nplanes; p++) { int st = atomic_load(&c.pl[p].status); if (st) { s = (crx_status)st; goto out; } d->overrun_bits += c.pl[p].overrun; }
    /* Phase 2: stages from level N down to level + 1 */
    for (uint32_t p = 0; p < nplanes; p++) {
        pp_plane *P = &c.pl[p]; const crx_plane *pl = &P->t->planes[P->pi];
        P->final = BROW(P->bands[0], &pl->bands[0], 0); P->final_w = pl->bands[0].width; P->final_h = pl->bands[0].height; P->final_s = BSTRIDE(&pl->bands[0]);
    }
    for (unsigned L = N; L > level; L--) {
        for (uint32_t p = 0; p < nplanes; p++) {
            pp_plane *P = &c.pl[p]; const crx_plane *pl = &P->t->planes[P->pi]; const crx_band *hb = &pl->bands[3 * (N - L) + 1];
            crx_stage *st = &P->st;
            st->ll = P->final; st->wl = P->final_w; st->hl = P->final_h; st->sll = P->final_s;
            st->hlb = BROW(P->bands[3 * (N - L) + 1], &hb[0], 0); st->whl = hb[0].width; st->hhl = hb[0].height; st->shl = BSTRIDE(&hb[0]);
            st->lhb = BROW(P->bands[3 * (N - L) + 2], &hb[1], 0); st->wlh = hb[1].width; st->hlh = hb[1].height; st->slh = BSTRIDE(&hb[1]);
            st->hhb = BROW(P->bands[3 * (N - L) + 3], &hb[2], 0); st->whh = hb[2].width; st->hhh = hb[2].height; st->shh = BSTRIDE(&hb[2]);
            st->left = P->t->flags & CRX_LEFT; st->right = P->t->flags & CRX_RIGHT; st->top = P->t->flags & CRX_TOP; st->bottom = P->t->flags & CRX_BOTTOM;
            st->out = P->outs[L]; st->m_w = (uint32_t)P->out_w[L]; st->m_h = (uint32_t)P->out_h[L]; st->so = P->out_w[L]; st->tmp = P->tmp;
        }
        make_blocks(&c, tot_rows, NULL, c.rows_per); crx_pool_run(d->pool, c.nblk, task_rows, &c);
        if (trace_on) { double T1 = now_ms(); fprintf(stderr, "stage %u rows (%u tasks) %.2f ms\n", L, c.nblk, T1 - T0); T0 = T1; }
        make_blocks(&c, tot_cols, NULL, c.cols_per); crx_pool_run(d->pool, c.nblk, task_cols, &c);
        if (trace_on) { double T1 = now_ms(); fprintf(stderr, "stage %u cols (%u tasks) %.2f ms\n", L, c.nblk, T1 - T0); T0 = T1; }
        for (uint32_t p = 0; p < nplanes; p++) { pp_plane *P = &c.pl[p]; P->final = P->outs[L]; P->final_w = (uint32_t)P->out_w[L]; P->final_h = (uint32_t)P->out_h[L]; P->final_s = P->out_w[L]; }
    }
    /* Phase 3: emit the own area */
    for (uint32_t p = 0; p < nplanes; p++) { pp_plane *P = &c.pl[p]; P->final_w = crx_ceil2n(P->t->w, level); P->final_h = crx_ceil2n(P->t->h, level); }
    make_blocks(&c, tot_emit, NULL, c.rows_per); crx_pool_run(d->pool, c.nblk, task_emit, &c);
    if (trace_on) { double T1 = now_ms(); fprintf(stderr, "emit (%u tasks) %.2f ms\n", c.nblk, T1 - T0); T0 = T1; }
out:
    for (uint32_t p = 0; p < nplanes; p++) crx_ws_put(c.pl[p].bands[0]);
    free(c.band_task); free(c.blk_plane); free(c.blk_start); free(c.blk_end);
out_small:
    if (qmem) for (uint32_t ti = 0; ti < ntiles; ti++) free(qmem[ti]);
    free(qms); free(qmem); free(c.pl);
    return s;
}

crx_status crx_decode_impl(crx_decoder *d, unsigned level, uint16_t *dst, size_t stride, unsigned threads)
{
    d->overrun_bits = 0;
    if (threads == 0) threads = 1;
    d->pool = crx_pool_shared(threads);
    if (!d->pool) return CRX_E_NOMEM;
    if (d->levels == 0) {
        if (level != 0) return CRX_E_ARG;
        for (uint32_t ti = 0; ti < d->tiles_x * d->tiles_y; ti++)
            for (uint32_t pi = 0; pi < d->nplanes; pi++) {
                crx_status s = decode_lossless_plane(d, &d->tiles[ti], pi, dst, stride);
                if (s != CRX_OK) return s;
            }
        return CRX_OK;
    }
    return decode_lossy_all(d, level, dst, stride);
}

static crx_status tile_qmap(crx_decoder *d, const crx_tile *t, crx_qmap *qm, uint32_t **qmem)
{
    *qmem = NULL;
    if (d->version != 0x200) return CRX_OK;
    if (t->qp_size == 0) return CRX_E_UNSUPPORTED;
    if (t->data_off + t->qp_size > d->len) return CRX_E_TRUNCATED;
    *qmem = malloc(crx_qmap_mem_size(t->w, t->h) * sizeof **qmem);
    if (!*qmem) return CRX_E_NOMEM;
    uint32_t over = 0;
    if (!crx_qmap_decode(d->bytes + t->data_off, t->qp_size, t->w, t->h, d->levels, qm, *qmem, &over)) { free(*qmem); *qmem = NULL; return CRX_E_CORRUPT; }
    d->overrun_bits += over;
    return CRX_OK;
}

crx_status crx_decode_plane_raw(crx_decoder *d, unsigned level, uint32_t tile, uint32_t plane, int32_t *raw, size_t stride)
{
    if (!d || !raw || tile >= d->tiles_x * d->tiles_y || plane >= d->nplanes || level > d->levels) return CRX_E_ARG;
    const crx_tile *t = &d->tiles[tile];
    if (d->levels == 0) {
        const crx_plane *pl = &t->planes[plane]; const crx_band *b = &pl->bands[0];
        if (b->data_off + b->data_size > d->len) return CRX_E_TRUNCATED;
        crx_linestate st; int32_t *mem = malloc(3 * ((size_t)t->w + 2) * sizeof *mem);
        if (!mem) return CRX_E_NOMEM;
        crx_line_init(&st, d->bytes + b->data_off, b->data_size, t->w, mem);
        for (uint32_t y = 0; y < t->h; y++) {
            const int32_t *line = pl->partial ? crx_line_ll(&st) : crx_line_hf(&st);
            if (!line) { free(mem); return CRX_E_CORRUPT; }
            memcpy(raw + (size_t)y * stride, line, t->w * sizeof *raw);
        }
        free(mem); return CRX_OK;
    }
    crx_qmap qm; uint32_t *qmem;
    crx_status s = tile_qmap(d, t, &qm, &qmem);
    if (s != CRX_OK) return s;
    s = decode_lossy_plane(d, t, plane, qmem ? &qm : NULL, level, 0, 0, NULL, 0, raw, stride, NULL, NULL, NULL);
    free(qmem);
    return s;
}

crx_status crx_decode_plane_ext(crx_decoder *d, unsigned level, uint32_t tile, uint32_t plane, int32_t **buf, uint32_t *ew, uint32_t *eh)
{
    if (!d || !buf || tile >= d->tiles_x * d->tiles_y || plane >= d->nplanes || level > d->levels) return CRX_E_ARG;
    const crx_tile *t = &d->tiles[tile];
    if (d->levels == 0) {
        *buf = malloc((size_t)t->w * t->h * sizeof **buf);
        if (!*buf) return CRX_E_NOMEM;
        crx_status s = crx_decode_plane_raw(d, 0, tile, plane, *buf, t->w);
        *ew = t->w; *eh = t->h;
        if (s != CRX_OK) { free(*buf); *buf = NULL; }
        return s;
    }
    crx_qmap qm; uint32_t *qmem;
    crx_status s = tile_qmap(d, t, &qm, &qmem);
    if (s != CRX_OK) return s;
    *buf = NULL;
    s = decode_lossy_plane(d, t, plane, qmem ? &qm : NULL, level, 0, 0, NULL, 0, NULL, 0, buf, ew, eh);
    free(qmem);
    return s;
}

crx_status crx_decode_band_ext(crx_decoder *d, uint32_t tile, uint32_t plane, unsigned k, int32_t **buf)
{
    if (!d || !buf || tile >= d->tiles_x * d->tiles_y || plane >= d->nplanes || k >= d->nbands) return CRX_E_ARG;
    const crx_tile *t = &d->tiles[tile]; const crx_plane *pl = &t->planes[plane]; const crx_band *b = &pl->bands[k];
    crx_qmap qm; uint32_t *qmem = NULL;
    crx_status s = d->levels ? tile_qmap(d, t, &qm, &qmem) : CRX_OK;
    if (s != CRX_OK) return s;
    int32_t *pad = malloc(BSIZE(b) * sizeof *pad);
    *buf = malloc((size_t)b->width * b->height * sizeof **buf);
    int32_t *mem = malloc(3 * ((size_t)b->width + 2) * sizeof *mem);
    if (!*buf || !mem || !pad) { free(*buf); free(mem); free(pad); free(qmem); return CRX_E_NOMEM; }
    s = decode_band(d, b, k == 0 && pl->partial, d->levels ? (k == 0 ? d->levels : d->levels - (k - 1) / 3) : 0, qmem ? &qm : NULL, pad, mem);
    if (s == CRX_OK) for (uint32_t y = 0; y < b->height; y++) memcpy(*buf + (size_t)y * b->width, BROW(pad, b, y), b->width * sizeof **buf);
    free(mem); free(qmem); free(pad);
    if (s != CRX_OK) { free(*buf); *buf = NULL; }
    return s;
}
