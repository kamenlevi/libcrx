/* SPDX-License-Identifier: Apache-2.0 */
#include "crx_internal.h"
#include "pool.h"
#include <stdlib.h>
#include <string.h>

#define CRX_VERSION "0.2.0"

static const uint8_t cfa_names[4][4] = { {0,1,1,2}, {1,0,2,1}, {1,2,0,1}, {2,1,1,0} };

crx_status crx_open(const void *bytes, size_t len, crx_decoder **out)
{
    if (!bytes || !out) return CRX_E_ARG;
    *out = NULL;
    crx_decoder *d = calloc(1, sizeof *d);
    if (!d) return CRX_E_NOMEM;
    d->bytes = bytes; d->len = len;
    const uint8_t *cmp1; size_t cmp1_len; uint32_t track;
    crx_status s = crx_find_image_track(d->bytes, len, &d->sample_off, &d->sample_size, &cmp1, &cmp1_len, &track);
    if (s == CRX_OK) s = crx_parse_cmp1(d, cmp1, cmp1_len);
    if (s == CRX_OK) s = crx_parse_codestream(d);
    if (s == CRX_OK) {
        uint32_t maxw = 0;
        for (uint32_t i = 0; i < d->tiles_x * d->tiles_y; i++) if (d->tiles[i].w > maxw) maxw = d->tiles[i].w;
        d->scratch_per_plane = 3 * ((size_t)maxw + 2);
        d->scratch = malloc(d->scratch_per_plane * d->nplanes * d->tiles_x * d->tiles_y * sizeof *d->scratch);
        if (!d->scratch) s = CRX_E_NOMEM;
    }
    if (s != CRX_OK) { crx_close(d); return s; }
    d->info.width = d->W; d->info.height = d->H;
    d->info.bits = d->bits; d->info.planes = d->nplanes; d->info.levels = d->levels;
    d->info.tiles_x = d->tiles_x; d->info.tiles_y = d->tiles_y;
    memcpy(d->info.cfa, cfa_names[d->cfa], 4);
    d->info.track = track;
    *out = d;
    return CRX_OK;
}

const crx_info *crx_get_info(const crx_decoder *d) { return d ? &d->info : NULL; }

size_t crx_output_size(const crx_decoder *d, unsigned level, uint32_t *w, uint32_t *h)
{
    if (!d || level > d->levels) return 0;
    uint32_t pw = 0, ph = 0;
    for (uint32_t tx = 0; tx < d->tiles_x; tx++) pw += crx_ceil2n(d->tiles[tx].w, level);
    for (uint32_t ty = 0; ty < d->tiles_y; ty++) ph += crx_ceil2n(d->tiles[ty * d->tiles_x].h, level);
    uint32_t ww = 2 * pw, hh = 2 * ph;
    if (w) *w = ww;
    if (h) *h = hh;
    return (size_t)ww * hh;
}

crx_status crx_decode(crx_decoder *d, unsigned level, uint16_t *dst, size_t stride, unsigned threads)
{
    if (!d || !dst) return CRX_E_ARG;
    if (level > d->levels) return CRX_E_ARG;
    uint32_t w; crx_output_size(d, level, &w, NULL);
    if (stride < w) return CRX_E_ARG;
    return crx_decode_impl(d, level, dst, stride, threads);
}

void crx_close(crx_decoder *d)
{
    if (!d) return;
    free(d->tiles);
    free(d->plane_storage);
    free(d->scratch);
    free(d);                              /* the pool is shared, never destroyed here */
}

const char *crx_strerror(crx_status s)
{
    switch (s) {
    case CRX_OK:            return "ok";
    case CRX_E_ARG:         return "bad argument";
    case CRX_E_FORMAT:      return "not a CR3 file";
    case CRX_E_UNSUPPORTED: return "unsupported variant";
    case CRX_E_CORRUPT:     return "corrupt data";
    case CRX_E_TRUNCATED:   return "truncated data";
    case CRX_E_NOMEM:       return "out of memory";
    }
    return "unknown status";
}

uint64_t crx_overrun_bits(const crx_decoder *d) { return d ? d->overrun_bits : 0; }

const char *crx_version(void) { return CRX_VERSION; }
