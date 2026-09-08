/* SPDX-License-Identifier: Apache-2.0
 * Decoding proper: lossless planes (milestone 3). SPEC 5, 9. */
#include "crx_internal.h"
#include "lines.h"
#include <stdlib.h>
#include <string.h>

/* Row and column offset inside the 2x2 cell for plane p under each CFA layout (SPEC 9). */
static const uint8_t cell_row[4][4] = { {0,0,1,1}, {0,0,1,1}, {1,1,0,0}, {1,1,0,0} };
static const uint8_t cell_col[4][4] = { {0,1,0,1}, {1,0,1,0}, {0,1,0,1}, {1,0,1,0} };

static crx_status decode_lossless_plane(crx_decoder *d, const crx_tile *t, uint32_t pi, uint16_t *dst, size_t stride)
{
    const crx_plane *pl = &t->planes[pi];
    const crx_band *b = &pl->bands[0];
    if (b->data_off + b->data_size > d->len) return CRX_E_TRUNCATED;
    crx_linestate st;
    crx_line_init(&st, d->bytes + b->data_off, b->data_size, t->w, d->scratch + (size_t)pi * d->scratch_per_plane);
    const int32_t median = 1 << (d->bits - 1), maxv = (1 << d->bits) - 1;
    uint32_t r0 = cell_row[d->cfa][pi], c0 = cell_col[d->cfa][pi];
    for (uint32_t y = 0; y < t->h; y++) {
        const int32_t *line = pl->partial ? crx_line_ll(&st) : crx_line_hf(&st);
        if (!line) return CRX_E_CORRUPT;
        uint16_t *row = dst + (size_t)(2 * (t->y0 + y) + r0) * stride + 2 * t->x0 + c0;
        for (uint32_t x = 0; x < t->w; x++) {
            int32_t v = median + line[x];
            row[2 * x] = (uint16_t)(v < 0 ? 0 : v > maxv ? maxv : v);
        }
    }
    d->overrun_bits += crx_bits_overrun_bits(&st.bits);
    return CRX_OK;
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
    return CRX_E_UNSUPPORTED;   /* milestone 4 */
}
