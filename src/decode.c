/* SPDX-License-Identifier: Apache-2.0
 * Decoding proper: lossless planes (SPEC 5, 9) and lossy planes (SPEC 7, 8, 10). */
#include "crx_internal.h"
#include "lines.h"
#include "qp.h"
#include "wavelet.h"
#include <stdlib.h>
#include <string.h>

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

/* Decode one whole band into buf (width x height), base-band or high-pass coder. */
static crx_status decode_band(crx_decoder *d, const crx_band *b, bool base, int32_t *buf, int32_t *mem)
{
    if (b->data_off + b->data_size > d->len) return CRX_E_TRUNCATED;
    if (b->data_size == 0) { memset(buf, 0, (size_t)b->width * b->height * sizeof *buf); return CRX_OK; }
    crx_linestate st;
    crx_line_init(&st, d->bytes + b->data_off, b->data_size, b->width, mem);
    for (uint32_t y = 0; y < b->height; y++) {
        const int32_t *line = base ? crx_line_ll(&st) : crx_line_hf(&st);
        if (!line) return CRX_E_CORRUPT;
        memcpy(buf + (size_t)y * b->width, line, b->width * sizeof *buf);
    }
    d->overrun_bits += crx_bits_overrun_bits(&st.bits);
    return CRX_OK;
}

/* SPEC 7.2 / 7.3: multiply the band's coefficients by their steps. `lvl` is 1 (finest) .. N. */
static crx_status dequantise(const crx_decoder *d, const crx_band *b, unsigned lvl, const crx_qmap *qm, int32_t *buf)
{
    if (d->version == 0x100) {
        if (b->q >= 36) return CRX_E_UNSUPPORTED;
        uint32_t m = crx_qstep((int32_t)b->q);
        if (m == 1) return CRX_OK;
        for (size_t i = 0, n = (size_t)b->width * b->height; i < n; i++) buf[i] = (int32_t)((uint32_t)buf[i] * m);
        return CRX_OK;
    }
    if (!qm) return CRX_E_UNSUPPORTED;
    const uint32_t *S = qm->S[lvl - 1]; uint32_t rows = qm->rows[lvl - 1], qw = qm->qw;
    unsigned sh = 3 - lvl;
    uint32_t own_w = b->width - b->left_extra - b->right_extra, own_h = b->height - b->top_extra - b->bottom_extra;
    for (uint32_t y = 0; y < b->height; y++) {
        uint32_t ry = y < b->top_extra ? 0 : (y - b->top_extra < own_h ? y - b->top_extra : own_h - 1);
        if (ry >= rows) ry = rows - 1;
        const uint32_t *srow = S + (size_t)ry * qw;
        int32_t *line = buf + (size_t)y * b->width;
        for (uint32_t x = 0; x < b->width; x++) {
            uint32_t rx = x < b->left_extra ? 0 : ((x - b->left_extra < own_w ? x - b->left_extra : own_w - 1) >> sh);
            if (rx >= qw) rx = qw - 1;
            int32_t m = (int32_t)b->qbase + (int32_t)((srow[rx] * b->qmult) >> 3);
            if (m < 1) m = 1; else if (m > 0x168000) m = 0x168000;
            line[x] = (int32_t)((uint32_t)line[x] * (uint32_t)m);
        }
    }
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
    for (unsigned k = 0; k < d->nbands; k++) need += (size_t)pl->bands[k].width * pl->bands[k].height;
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
    for (unsigned k = 0; k < d->nbands; k++) { bands[k] = p; p += (size_t)pl->bands[k].width * pl->bands[k].height; }
    for (unsigned L = 1; L <= N; L++) { outs[L] = p; p += out_w[L] * out_h[L]; }
    int32_t *linemem = p; p += 3 * ((size_t)maxw + 2);
    int32_t *tmp = p;

    crx_status s = CRX_OK;
    for (unsigned k = 0; k < d->nbands && s == CRX_OK; k++) {
        const crx_band *b = &pl->bands[k];
        unsigned lvl = k == 0 ? N : N - (k - 1) / 3;              /* band index -> wavelet level */
        s = decode_band(d, b, k == 0 && pl->partial, bands[k], linemem);
        if (s == CRX_OK) s = dequantise(d, b, lvl, qm, bands[k]);
    }
    /* Stages from the coarsest level N down to level `level` + 1. */
    const int32_t *ll = bands[0]; uint32_t wl = pl->bands[0].width, hl = pl->bands[0].height;
    for (unsigned L = N; L > level && s == CRX_OK; L--) {
        const crx_band *hb = &pl->bands[3 * (N - L) + 1];
        crx_synth_stage(ll, wl, hl, wl,
                        bands[3 * (N - L) + 1], hb[0].width, hb[0].height, hb[0].width,
                        bands[3 * (N - L) + 2], hb[1].width, hb[1].height, hb[1].width,
                        bands[3 * (N - L) + 3], hb[2].width, hb[2].height, hb[2].width,
                        t->flags & CRX_LEFT, t->flags & CRX_RIGHT, t->flags & CRX_TOP, t->flags & CRX_BOTTOM,
                        outs[L], (uint32_t)out_w[L], (uint32_t)out_h[L], out_w[L], tmp);
        ll = outs[L]; wl = (uint32_t)out_w[L]; hl = (uint32_t)out_h[L];
    }
    if (s == CRX_OK) {
        uint32_t ow = crx_ceil2n(t->w, level), oh = crx_ceil2n(t->h, level);
        if (dst) emit_plane(d, pi, ll, wl, ow, oh, px_level, py_level, dst, stride);
        if (raw) for (uint32_t y = 0; y < oh; y++) memcpy(raw + (size_t)y * raw_stride, ll + (size_t)y * wl, ow * sizeof *raw);
        if (ext) {
            *ext = malloc((size_t)wl * hl * sizeof **ext);
            if (*ext) { memcpy(*ext, ll, (size_t)wl * hl * sizeof **ext); *ew = wl; *eh = hl; } else s = CRX_E_NOMEM;
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
    *buf = malloc((size_t)b->width * b->height * sizeof **buf);
    int32_t *mem = malloc(3 * ((size_t)b->width + 2) * sizeof *mem);
    if (!*buf || !mem) { free(*buf); free(mem); free(qmem); return CRX_E_NOMEM; }
    s = decode_band(d, b, k == 0 && pl->partial, *buf, mem);
    if (s == CRX_OK && d->levels) s = dequantise(d, b, k == 0 ? d->levels : d->levels - (k - 1) / 3, qmem ? &qm : NULL, *buf);
    free(mem); free(qmem);
    if (s != CRX_OK) { free(*buf); *buf = NULL; }
    return s;
}
