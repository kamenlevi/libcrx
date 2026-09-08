/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CRX_INTERNAL_H
#define CRX_INTERNAL_H
#include "crx.h"
#include <stdbool.h>

#define CRX_MAX_LEVELS 3
#define CRX_MAX_BANDS  (3 * CRX_MAX_LEVELS + 1)

enum { CRX_LEFT = 1, CRX_RIGHT = 2, CRX_TOP = 4, CRX_BOTTOM = 8 };

/* One subband of one plane of one tile. width/height count stored
 * coefficients, seam extras included (SPEC 4.2). */
typedef struct crx_band {
    uint32_t width, height;
    uint8_t  left_extra, top_extra;       /* 0 or 1: a column/row before index 0 */
    uint8_t  right_extra, bottom_extra;   /* 0..2: columns/rows after the tile's own */
    uint64_t data_off;                    /* absolute file offset of the coded bytes */
    uint32_t data_size;                   /* bytes the coder may read (coded_size - tail) */
    uint32_t coded_size;                  /* bytes occupied in the file */
    uint32_t q;                           /* v1 quantisation parameter */
    uint32_t qbase, qmult;                /* v2 */
} crx_band;

typedef struct crx_plane {
    uint64_t data_off;
    uint32_t size;
    uint8_t  partial, rounded;
    crx_band bands[CRX_MAX_BANDS];
} crx_plane;

typedef struct crx_tile {
    uint32_t x0, y0, w, h;                /* position and size in plane units */
    uint8_t  flags;
    uint64_t data_off;                    /* start of the tile's data (QP table first in v2) */
    uint32_t size;
    uint32_t qp_size;
    uint16_t extra;
    crx_plane *planes;                    /* nplanes entries */
} crx_tile;

struct crx_decoder {
    const uint8_t *bytes;
    size_t len;
    crx_info info;
    uint16_t version;                     /* 0x100 or 0x200 */
    uint32_t W, H, TW, TH;                /* CMP1, in image (mosaic) units */
    uint32_t PW, PH, PTW, PTH;            /* the same in plane units */
    uint8_t  bits, nplanes, cfa, enc, levels, nbands;
    uint32_t hdr_size;
    uint64_t sample_off, sample_size;
    uint32_t tiles_x, tiles_y;
    crx_tile *tiles;
    crx_plane *plane_storage;
    int32_t *scratch;                     /* line-decoder memory, nplanes x scratch_per_plane int32 */
    size_t scratch_per_plane;
    uint64_t overrun_bits;                /* bits read past band data in the last decode */
};
crx_status crx_decode_impl(crx_decoder *d, unsigned level, uint16_t *dst, size_t stride, unsigned threads);
/* Diagnostics for tests and crxcheck -p: one tile plane at `level`, unclamped,
 * into raw (ceil2n(w) x ceil2n(h), row stride `stride`). */
crx_status crx_decode_plane_raw(crx_decoder *d, unsigned level, uint32_t tile, uint32_t plane, int32_t *raw, size_t stride);
/* The same, but the whole stage output including seam extras (and, at level 0,
 * one virtual sample beyond an even-sized seam edge). Caller frees *buf. */
crx_status crx_decode_plane_ext(crx_decoder *d, unsigned level, uint32_t tile, uint32_t plane, int32_t **buf, uint32_t *ew, uint32_t *eh);
/* One dequantised band (geometry in d->tiles[tile].planes[plane].bands[k]). Caller frees *buf. */
crx_status crx_decode_band_ext(crx_decoder *d, uint32_t tile, uint32_t plane, unsigned k, int32_t **buf);

/* container.c */
crx_status crx_find_image_track(const uint8_t *buf, size_t len, uint64_t *sample_off, uint64_t *sample_size,
                                const uint8_t **cmp1, size_t *cmp1_len, uint32_t *track);
/* headers.c */
crx_status crx_parse_cmp1(crx_decoder *d, const uint8_t *cmp1, size_t cmp1_len);
crx_status crx_parse_codestream(crx_decoder *d);
void       crx_seam_extras(unsigned levels, uint32_t n, uint8_t ex_h[CRX_MAX_LEVELS], uint8_t ex_l[CRX_MAX_LEVELS]);
uint32_t   crx_ceil2n(uint32_t n, unsigned times);

static inline uint32_t crx_rd16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static inline uint32_t crx_rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline uint64_t crx_rd64(const uint8_t *p) { return (uint64_t)crx_rd32(p) << 32 | crx_rd32(p + 4); }
#endif
