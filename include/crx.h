/*
 * libcrx: a decoder for Canon's CRX raw image codec, as found in CR3 files.
 *
 * Copyright 2026 Kamen Levi
 * SPDX-License-Identifier: Apache-2.0
 *
 * Scope: the container walk needed to find the sensor data, the CRX headers,
 * and the sensor values themselves, exactly. No demosaic, no colour.
 * Everything is integer. The library allocates nothing on the decode path;
 * the caller owns the output buffer.
 */
#ifndef CRX_H
#define CRX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum crx_status {
    CRX_OK = 0,
    CRX_E_ARG,          /* a NULL or out-of-range argument */
    CRX_E_FORMAT,       /* not a CR3 / no CRX image inside */
    CRX_E_UNSUPPORTED,  /* recognised, but a variant this build does not decode */
    CRX_E_CORRUPT,      /* the data contradicts itself */
    CRX_E_TRUNCATED,    /* the data ends early */
    CRX_E_NOMEM
} crx_status;

typedef struct crx_info {
    uint32_t width;      /* sensor plane width at level 0, in photosites */
    uint32_t height;
    uint32_t bits;       /* bit depth of the sensor values */
    uint32_t planes;     /* colour planes stored by the codec (4 for a Bayer sensor) */
    uint32_t levels;     /* wavelet levels; 0 means lossless */
    uint32_t tiles_x;
    uint32_t tiles_y;
    uint8_t  cfa[4];     /* CFA pattern of the 2x2 cell, row-major: 0 R, 1 G, 2 B */
    uint32_t track;      /* index of the CR3 track that carried the image */
    /* Rectangles Canon writes in the IAD1 box, in level-0 mosaic pixels, as
     * x, y, width, height. `active` is the sensor area with image data;
     * `crop` is the recommended crop (the camera's advertised size). Both
     * are zero when the box is absent (small preview tracks carry a shorter
     * box with only the crop). LibRaw's margins differ from `active` by up
     * to two pixels; they come from a different tag. */
    uint32_t active[4];
    uint32_t crop[4];
} crx_info;

typedef enum crx_track { CRX_TRACK_MAIN = 0, CRX_TRACK_PREVIEW = 1 } crx_track;

typedef struct crx_decoder crx_decoder;

/* Parse the container and every CRX header. Does not decode pixels.
 * The bytes must stay valid until crx_close. */
crx_status crx_open(const void *bytes, size_t len, crx_decoder **out);
/* The same, choosing the largest CRX track (main) or the smallest (the
 * 1624x1080-class preview many bodies write, itself a CRX image). */
crx_status crx_open_track(const void *bytes, size_t len, crx_track which, crx_decoder **out);

const crx_info *crx_get_info(const crx_decoder *d);

/* Output geometry for a reduction level: 0 = full size, 1 = half in each
 * direction, and so on, up to info->levels. Returns the size in pixels of the
 * buffer crx_decode needs (width * height at that level), or 0 if the level is
 * not available for this image. */
size_t crx_output_size(const crx_decoder *d, unsigned level, uint32_t *w, uint32_t *h);

/* Decode into dst, row-major, one uint16 per photosite, rows `stride` pixels
 * apart (stride >= the level's width). Level as for crx_output_size.
 * `threads` is the number of workers including the caller; 0 or 1 decodes
 * on the calling thread alone. Workers live in process-wide pools, one per
 * distinct count ever requested (at most four; further counts reuse the
 * largest); several threads may each decode their own decoder at the same
 * time (runs on one pool take turns). A decoder object must not be used by two threads at once.
 * A level > 0 result is, by definition, the low-pass band of the exact
 * integer 5/3 analysis of the level 0 result, applied `level` times, per
 * tile in the tile's own frame (SPEC section 10). Errors leave dst
 * partially written. */
crx_status crx_decode(crx_decoder *d, unsigned level, uint16_t *dst, size_t stride, unsigned threads);

void crx_close(crx_decoder *d);

const char *crx_strerror(crx_status s);

/* Diagnostics: bits the last crx_decode read beyond the coded data of its
 * subbands (a conforming file gives 0; the corpus check asserts it). */
uint64_t crx_overrun_bits(const crx_decoder *d);
const char *crx_version(void);

#ifdef __cplusplus
}
#endif
#endif
