/* SPDX-License-Identifier: Apache-2.0
 * Decoding proper: lossless planes (SPEC 5, 9) and lossy planes (SPEC 7, 8, 10). */
#include "crx_internal.h"
#include "lines.h"
#include "qp.h"
#include "wavelet.h"
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

crx_status crx_decode_impl(crx_decoder *d, unsigned level, uint16_t *dst, size_t stride, unsigned threads)
{
    (void)threads;
    d->overrun_bits = 0;
    if (d->levels == 0) {
        if (level != 0) return CRX_E_ARG;
        for (uint32_t ti = 0; ti < d->tiles_x * d->tiles_y; ti++)
            for (uint32_t pi = 0; pi < d->nplanes; pi++) {
                crx_status s = decode_lossless_plane(d, &d->tiles[ti], pi, dst, stride);
                if (s != CRX_OK) return s;
            }
        return CRX_OK;
    }
    uint32_t py = 0;
    for (uint32_t ty = 0; ty < d->tiles_y; ty++) {
        uint32_t px = 0, th = 0;
        for (uint32_t tx = 0; tx < d->tiles_x; tx++) {
            crx_tile *t = &d->tiles[ty * d->tiles_x + tx];
            crx_qmap qm, *qmp = NULL; uint32_t *qmem = NULL;
            if (d->version == 0x200) {
                if (t->qp_size == 0) return CRX_E_UNSUPPORTED;
                if (t->data_off + t->qp_size > d->len) return CRX_E_TRUNCATED;
                qmem = malloc(crx_qmap_mem_size(t->w, t->h) * sizeof *qmem);
                if (!qmem) return CRX_E_NOMEM;
                uint32_t over = 0;
                if (!crx_qmap_decode(d->bytes + t->data_off, t->qp_size, t->w, t->h, d->levels, &qm, qmem, &over)) { free(qmem); return CRX_E_CORRUPT; }
                d->overrun_bits += over;
                qmp = &qm;
            }
            for (uint32_t pi = 0; pi < d->nplanes; pi++) {
                crx_status s = decode_lossy_plane(d, t, pi, qmp, level, px, py, dst, stride, NULL, 0, NULL, NULL, NULL);
                if (s != CRX_OK) { free(qmem); return s; }
            }
            free(qmem);
            px += crx_ceil2n(t->w, level); th = crx_ceil2n(t->h, level);
        }
        py += th;
    }
    return CRX_OK;
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
