/* SPDX-License-Identifier: Apache-2.0
 * CMP1 image header, codestream headers and subband geometry.
 * SPEC sections 2, 3 and 4. */
#include "crx_internal.h"
#include <stdlib.h>
#include <string.h>

uint32_t crx_ceil2n(uint32_t n, unsigned times)
{
    while (times--) n = (n + 1) >> 1;
    return n;
}

/* SPEC 4.2: extras stored on the side of a seam, per level (index 0 = finest).
 * ex_h applies to bands that are high-pass along this axis, ex_l to low-pass
 * ones (and to LL at the coarsest level). Least fixed point of need / produce
 * / chain, iterated from "no extras". */
void crx_seam_extras(unsigned levels, uint32_t n0, uint8_t ex_h[CRX_MAX_LEVELS], uint8_t ex_l[CRX_MAX_LEVELS])
{
    uint32_t n[CRX_MAX_LEVELS + 1], m[CRX_MAX_LEVELS], L[CRX_MAX_LEVELS], H[CRX_MAX_LEVELS];
    n[0] = n0;
    for (unsigned l = 0; l < levels; l++) n[l + 1] = (n[l] + 1) >> 1;
    for (unsigned l = 0; l < levels; l++) m[l] = n[l];
    for (int round = 0; round < 8; round++) {
        for (unsigned l = 0; l < levels; l++) {
            uint32_t k = m[l];
            L[l] = H[l] = (k & 1) ? (k + 1) >> 1 : k / 2 + 1;           /* need */
        }
        for (int l = (int)levels - 2; l >= 0; l--) {                    /* produce */
            uint32_t prod = 2 * (L[l + 1] < H[l + 1] ? L[l + 1] : H[l + 1]) - 1;
            if (prod > L[l]) L[l] = prod;
        }
        bool same = true;
        for (unsigned l = 1; l < levels; l++) {                          /* chain */
            uint32_t want = n[l] + (L[l - 1] - ((n[l - 1] + 1) >> 1));
            if (want != m[l]) { m[l] = want; same = false; }
        }
        if (same) break;
    }
    for (unsigned l = 0; l < levels; l++) {
        ex_h[l] = (uint8_t)(H[l] - n[l] / 2);
        ex_l[l] = (uint8_t)(L[l] - ((n[l] + 1) >> 1));
    }
}

crx_status crx_parse_cmp1(crx_decoder *d, const uint8_t *c, size_t n)
{
    if (n < 52) return CRX_E_TRUNCATED;
    d->version = (uint16_t)crx_rd16(c + 4);
    d->W = crx_rd32(c + 8); d->H = crx_rd32(c + 12);
    d->TW = crx_rd32(c + 16); d->TH = crx_rd32(c + 20);
    d->bits = c[24];
    d->nplanes = c[25] >> 4; d->cfa = c[25] & 15;
    d->enc = c[26] >> 4; d->levels = c[26] & 15;
    d->hdr_size = crx_rd32(c + 28);
    bool tile_cols = (c[27] >> 7) & 1, tile_rows = (c[27] >> 6) & 1;
    bool ext = (c[32] >> 7) & 1;

    if (d->version != 0x100 && d->version != 0x200) return CRX_E_UNSUPPORTED;
    if (!d->W || !d->H || !d->TW || !d->TH || d->W > 0xFFFF || d->H > 0xFFFF) return CRX_E_CORRUPT;
    if (d->TW > d->W || d->TH > d->H || !d->hdr_size) return CRX_E_CORRUPT;
    if (d->levels > CRX_MAX_LEVELS) return CRX_E_CORRUPT;
    /* Scope (SPEC 12): four planes, 14 bits, encoding 0, N = 0 or 3, no ext header, one tile row. */
    if (d->nplanes != 4 || d->bits != 14 || d->enc != 0 || (d->levels != 0 && d->levels != 3) || ext || tile_rows)
        return CRX_E_UNSUPPORTED;
    if (d->cfa > 3) return CRX_E_CORRUPT;
    if ((d->W | d->H | d->TW | d->TH) & 1) return CRX_E_CORRUPT;
    d->PW = d->W / 2; d->PH = d->H / 2; d->PTW = d->TW / 2; d->PTH = d->TH / 2;
    d->tiles_x = (d->PW + d->PTW - 1) / d->PTW;
    d->tiles_y = (d->PH + d->PTH - 1) / d->PTH;
    if (d->tiles_x > 2 || d->tiles_y != 1) return CRX_E_UNSUPPORTED;
    (void)tile_cols;   /* lossless two-tile files leave the flag clear; the sizes decide */
    if (d->PTW < 11 || d->PW - d->PTW * (d->tiles_x - 1) < 11 || d->PTH < 11) return CRX_E_CORRUPT;
    d->nbands = (uint8_t)(3 * d->levels + 1);
    return CRX_OK;
}

/* Geometry of every band of one tile plane (SPEC 4). */
static void band_geometry(const crx_decoder *d, crx_tile *t, crx_plane *pl)
{
    unsigned N = d->levels;
    if (N == 0) {
        crx_band *b = &pl->bands[0];
        memset(b, 0, sizeof *b);
        b->width = t->w; b->height = t->h;
        return;
    }
    uint8_t exh_w[CRX_MAX_LEVELS] = {0}, exl_w[CRX_MAX_LEVELS] = {0};
    uint8_t exh_h[CRX_MAX_LEVELS] = {0}, exl_h[CRX_MAX_LEVELS] = {0};
    if (t->flags & CRX_RIGHT)  crx_seam_extras(N, t->w, exh_w, exl_w);
    if (t->flags & CRX_BOTTOM) crx_seam_extras(N, t->h, exh_h, exl_h);
    uint8_t left = (t->flags & CRX_LEFT) ? 1 : 0, top = (t->flags & CRX_TOP) ? 1 : 0;

    uint32_t n = t->w, m = t->h;
    for (unsigned l = 0; l < N; l++) {                /* l = 0 finest; bands HL LH HH at index 3*(N-1-l)+1.. */
        uint32_t fl_n = n / 2, ce_n = (n + 1) / 2, fl_m = m / 2, ce_m = (m + 1) / 2;
        crx_band *hl = &pl->bands[3 * (N - 1 - l) + 1], *lh = hl + 1, *hh = hl + 2;
        memset(hl, 0, 3 * sizeof *hl);
        hl->width = fl_n + exh_w[l] + left; hl->left_extra = left; hl->right_extra = exh_w[l];
        hl->height = ce_m + exl_h[l];                  hl->bottom_extra = exl_h[l];
        lh->width = ce_n + exl_w[l];                   lh->right_extra = exl_w[l];
        lh->height = fl_m + exh_h[l] + top; lh->top_extra = top; lh->bottom_extra = exh_h[l];
        hh->width = fl_n + exh_w[l] + left; hh->left_extra = left; hh->right_extra = exh_w[l];
        hh->height = fl_m + exh_h[l] + top; hh->top_extra = top; hh->bottom_extra = exh_h[l];
        n = ce_n; m = ce_m;
    }
    crx_band *ll = &pl->bands[0];
    memset(ll, 0, sizeof *ll);
    ll->width = n + exl_w[N - 1]; ll->right_extra = exl_w[N - 1];
    ll->height = m + exl_h[N - 1]; ll->bottom_extra = exl_h[N - 1];
}

crx_status crx_parse_codestream(crx_decoder *d)
{
    if (d->sample_size < d->hdr_size) return CRX_E_TRUNCATED;
    const uint8_t *p = d->bytes + d->sample_off, *end = p + d->hdr_size;
    uint32_t ntiles = d->tiles_x * d->tiles_y;
    d->tiles = calloc(ntiles, sizeof *d->tiles);
    d->plane_storage = calloc((size_t)ntiles * d->nplanes, sizeof *d->plane_storage);
    if (!d->tiles || !d->plane_storage) return CRX_E_NOMEM;

    uint64_t tile_off = d->sample_off + d->hdr_size;
    uint64_t sample_end = d->sample_off + d->sample_size;
    bool v2 = d->version == 0x200;
    uint16_t mk_tile = v2 ? 0xFF11 : 0xFF01, mk_plane = v2 ? 0xFF12 : 0xFF02, mk_band = v2 ? 0xFF13 : 0xFF03;

    for (uint32_t ti = 0; ti < ntiles; ti++) {
        crx_tile *t = &d->tiles[ti];
        t->planes = d->plane_storage + (size_t)ti * d->nplanes;
        uint32_t tx = ti % d->tiles_x, ty = ti / d->tiles_x;
        t->x0 = tx * d->PTW; t->y0 = ty * d->PTH;
        t->w = (tx == d->tiles_x - 1) ? d->PW - t->x0 : d->PTW;
        t->h = (ty == d->tiles_y - 1) ? d->PH - t->y0 : d->PTH;
        t->flags = (uint8_t)((tx ? CRX_LEFT : 0) | (tx + 1 < d->tiles_x ? CRX_RIGHT : 0) |
                             (ty ? CRX_TOP : 0) | (ty + 1 < d->tiles_y ? CRX_BOTTOM : 0));

        if (end - p < 12) return CRX_E_TRUNCATED;
        uint32_t hlen = crx_rd16(p + 2);
        if (crx_rd16(p) != mk_tile || (hlen != 8 && !(v2 && hlen == 16))) return CRX_E_CORRUPT;
        if ((size_t)(end - p) < 4 + hlen) return CRX_E_TRUNCATED;
        t->size = crx_rd32(p + 4);
        if (crx_rd16(p + 8) != ti) return CRX_E_CORRUPT;
        if (hlen == 8) {
            if (crx_rd16(p + 10) != 0) return CRX_E_CORRUPT;
        } else {
            if (crx_rd16(p + 10) != 0x4000 || crx_rd16(p + 18) != 0) return CRX_E_CORRUPT;
            t->qp_size = crx_rd32(p + 12);
            t->extra = (uint16_t)crx_rd16(p + 16);
        }
        t->data_off = tile_off;
        if (t->size > sample_end - tile_off) return CRX_E_TRUNCATED;
        if ((uint64_t)t->qp_size + t->extra > t->size) return CRX_E_CORRUPT;
        p += 4 + hlen;

        uint64_t plane_off = tile_off + t->qp_size + t->extra;
        uint64_t planes_total = 0;
        for (uint32_t pi = 0; pi < d->nplanes; pi++) {
            crx_plane *pl = &t->planes[pi];
            if (end - p < 12) return CRX_E_TRUNCATED;
            if (crx_rd16(p) != mk_plane || crx_rd16(p + 2) != 8) return CRX_E_CORRUPT;
            pl->size = crx_rd32(p + 4);
            if ((p[8] >> 4) != pi || (p[9] | p[10] | p[11])) return CRX_E_CORRUPT;
            pl->partial = (p[8] >> 3) & 1;
            pl->rounded = (p[8] >> 1) & 3;
            if (pl->rounded) return CRX_E_UNSUPPORTED;          /* SPEC 6 */
            pl->data_off = plane_off;
            planes_total += pl->size;
            if (planes_total + t->qp_size + t->extra > t->size) return CRX_E_CORRUPT;
            p += 12;

            band_geometry(d, t, pl);
            uint64_t band_off = plane_off, bands_total = 0;
            for (uint32_t bi = 0; bi < d->nbands; bi++) {
                crx_band *b = &pl->bands[bi];
                if (end - p < 12) return CRX_E_TRUNCATED;
                uint32_t blen = crx_rd16(p + 2);
                if (crx_rd16(p) != mk_band || blen != (v2 ? 16u : 8u)) return CRX_E_CORRUPT;
                if ((size_t)(end - p) < 4 + blen) return CRX_E_TRUNCATED;
                b->coded_size = crx_rd32(p + 4);
                if ((p[8] >> 4) != bi) return CRX_E_CORRUPT;
                uint32_t tail;
                if (!v2) {
                    uint32_t packed = crx_rd32(p + 8);
                    if ((packed >> 27) & 1) return CRX_E_UNSUPPORTED;   /* per-subband partial q, SPEC 6 */
                    b->q = (packed >> 19) & 0xFF;
                    tail = packed & 0x7FFFF;
                    b->qbase = 0; b->qmult = 0;
                } else {
                    if ((crx_rd16(p + 8) & 0xFFF) || crx_rd16(p + 18)) return CRX_E_CORRUPT;
                    b->qmult = crx_rd16(p + 10);
                    b->qbase = crx_rd32(p + 12);
                    tail = crx_rd16(p + 16);
                    b->q = 0;
                }
                if (tail > b->coded_size) return CRX_E_CORRUPT;
                b->data_size = b->coded_size - tail;
                b->data_off = band_off;
                band_off += b->coded_size;
                bands_total += b->coded_size;
                if (bands_total > pl->size) return CRX_E_CORRUPT;
                p += 4 + blen;
            }
            if (bands_total != pl->size) return CRX_E_CORRUPT;
            plane_off += pl->size;
        }
        if (planes_total + t->qp_size + t->extra != t->size) return CRX_E_CORRUPT;
        tile_off += t->size;
    }
    if (tile_off > sample_end) return CRX_E_TRUNCATED;
    return CRX_OK;
}
