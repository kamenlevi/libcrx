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
    uint32_t track;      /* which CR3 track carried the image (0 = the full-size raw) */
} crx_info;

typedef struct crx_decoder crx_decoder;

/* Parse the container and every CRX header. Does not decode pixels.
 * The bytes must stay valid until crx_close. */
crx_status crx_open(const void *bytes, size_t len, crx_decoder **out);

const crx_info *crx_get_info(const crx_decoder *d);

/* Output geometry for a reduction level: 0 = full size, 1 = half in each
 * direction, and so on, up to info->levels. Returns the size in pixels of the
 * buffer crx_decode needs (width * height at that level), or 0 if the level is
 * not available for this image. */
size_t crx_output_size(const crx_decoder *d, unsigned level, uint32_t *w, uint32_t *h);

/* Decode into dst, row-major, one uint16 per photosite, rows `stride` pixels
 * apart. Level as for crx_output_size. `threads` 0 means one thread.
 * A level > 0 result is, by definition, the LL band of the exact integer
 * wavelet analysis of the level 0 result, repeated `level` times. */
crx_status crx_decode(crx_decoder *d, unsigned level, uint16_t *dst, size_t stride, unsigned threads);

void crx_close(crx_decoder *d);

const char *crx_strerror(crx_status s);
const char *crx_version(void);

#ifdef __cplusplus
}
#endif
#endif
