/* SPDX-License-Identifier: Apache-2.0
 * A tiny builder for synthetic CR3 files, for tests and fuzz seeds. */
#ifndef CRX_MKCR3_H
#define CRX_MKCR3_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *p; size_t n, cap; } bb;

static void bb_put(bb *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void bb_u8(bb *b, unsigned v) { uint8_t x = (uint8_t)v; bb_put(b, &x, 1); }
static void bb_u16(bb *b, unsigned v) { bb_u8(b, v >> 8); bb_u8(b, v); }
static void bb_u32(bb *b, uint32_t v) { bb_u16(b, v >> 16); bb_u16(b, v & 0xFFFF); }
static void bb_u64(bb *b, uint64_t v) { bb_u32(b, (uint32_t)(v >> 32)); bb_u32(b, (uint32_t)v); }
static void bb_zeros(bb *b, size_t n) { while (n--) bb_u8(b, 0); }
static size_t bb_open(bb *b, const char *type) { size_t at = b->n; bb_u32(b, 0); bb_put(b, type, 4); return at; }
static void bb_close(bb *b, size_t at)
{
    uint32_t size = (uint32_t)(b->n - at);
    b->p[at] = (uint8_t)(size >> 24); b->p[at+1] = (uint8_t)(size >> 16); b->p[at+2] = (uint8_t)(size >> 8); b->p[at+3] = (uint8_t)size;
}

typedef struct {
    unsigned version;            /* 0x100 or 0x200 */
    uint32_t W, H, TW, TH;       /* image units */
    unsigned bits, planes, cfa, enc, levels;
    uint32_t hdr_size;           /* codestream header size; 0 = compute */
} mk_cmp1;

/* CMP1 payload (52 bytes after the box header) per SPEC 2. */
static void bb_cmp1(bb *b, const mk_cmp1 *c, uint32_t hdr_size)
{
    size_t at = bb_open(b, "CMP1");
    bb_u16(b, 0xFFFF); bb_u16(b, 0x30); bb_u16(b, c->version); bb_u16(b, 0);
    bb_u32(b, c->W); bb_u32(b, c->H); bb_u32(b, c->TW); bb_u32(b, c->TH);
    bb_u8(b, c->bits); bb_u8(b, (c->planes << 4) | c->cfa); bb_u8(b, (c->enc << 4) | c->levels);
    bb_u8(b, ((c->TW < c->W) ? 0x80 : 0) | ((c->TH < c->H) ? 0x40 : 0));
    bb_u32(b, hdr_size);
    bb_u8(b, 0); bb_u8(b, 0); bb_u16(b, 0);
    for (int i = 0; i < 4; i++) { bb_u8(b, 1); bb_u8(b, 1); bb_u16(b, 0); }
    bb_close(b, at);
}

/* A track with a CRAW sample entry, stsz and co64. Offsets patched later. */
static size_t bb_track(bb *b, const mk_cmp1 *c, uint32_t hdr_size, uint32_t sample_size, size_t *co64_pos)
{
    size_t trak = bb_open(b, "trak");
    size_t mdia = bb_open(b, "mdia");
    size_t minf = bb_open(b, "minf");
    size_t stbl = bb_open(b, "stbl");
    size_t stsd = bb_open(b, "stsd"); bb_u32(b, 0); bb_u32(b, 1);
    size_t craw = bb_open(b, "CRAW");
    bb_zeros(b, 24); bb_u16(b, c->W); bb_u16(b, c->H); bb_zeros(b, 82 - 28);
    bb_cmp1(b, c, hdr_size);
    bb_close(b, craw); bb_close(b, stsd);
    size_t stsz = bb_open(b, "stsz"); bb_u32(b, 0); bb_u32(b, sample_size); bb_u32(b, 1); bb_close(b, stsz);
    size_t co64 = bb_open(b, "co64"); bb_u32(b, 0); bb_u32(b, 1); *co64_pos = b->n; bb_u64(b, 0); bb_close(b, co64);
    bb_close(b, stbl); bb_close(b, minf); bb_close(b, mdia); bb_close(b, trak);
    return trak;
}

/* Whole file: ftyp, moov with one CRX track, mdat holding `sample`. */
static bb mk_file(const mk_cmp1 *c, uint32_t hdr_size, const uint8_t *sample, uint32_t sample_size)
{
    bb b = {0};
    size_t ftyp = bb_open(&b, "ftyp"); bb_put(&b, "crx ", 4); bb_u32(&b, 1); bb_put(&b, "crx isom", 8); bb_close(&b, ftyp);
    size_t moov = bb_open(&b, "moov");
    size_t co64_pos;
    bb_track(&b, c, hdr_size, sample_size, &co64_pos);
    bb_close(&b, moov);
    size_t mdat = bb_open(&b, "mdat");
    uint64_t off = b.n;
    bb_put(&b, sample, sample_size);
    bb_close(&b, mdat);
    for (int i = 0; i < 8; i++) b.p[co64_pos + i] = (uint8_t)(off >> (56 - 8 * i));
    return b;
}

/* Codestream headers for one tile, `planes` planes, `nbands` bands of the given
 * byte sizes (band_sizes[plane][band]), v1 or v2. Returns bytes appended. */
static void bb_codestream(bb *b, unsigned version, unsigned tile_index, unsigned planes, unsigned nbands,
                          const uint32_t *band_sizes, const uint32_t *q_or_qmult, const uint32_t *qbase,
                          uint32_t qp_size, unsigned extra)
{
    int v2 = version == 0x200;
    uint32_t tile_size = qp_size + extra;
    for (unsigned p = 0; p < planes; p++) for (unsigned k = 0; k < nbands; k++) tile_size += band_sizes[p * nbands + k];
    bb_u16(b, v2 ? 0xFF11 : 0xFF01); bb_u16(b, v2 ? 16 : 8); bb_u32(b, tile_size); bb_u16(b, tile_index);
    if (v2) { bb_u16(b, 0x4000); bb_u32(b, qp_size); bb_u16(b, extra); bb_u16(b, 0); } else bb_u16(b, 0);
    for (unsigned p = 0; p < planes; p++) {
        uint32_t psize = 0;
        for (unsigned k = 0; k < nbands; k++) psize += band_sizes[p * nbands + k];
        bb_u16(b, v2 ? 0xFF12 : 0xFF02); bb_u16(b, 8); bb_u32(b, psize); bb_u8(b, (p << 4) | 8); bb_zeros(b, 3);
        for (unsigned k = 0; k < nbands; k++) {
            uint32_t sz = band_sizes[p * nbands + k];
            bb_u16(b, v2 ? 0xFF13 : 0xFF03); bb_u16(b, v2 ? 16 : 8); bb_u32(b, sz);
            if (!v2) bb_u32(b, (k << 28) | ((q_or_qmult ? q_or_qmult[k] : 4u) << 19) | 0);
            else { bb_u8(b, k << 4); bb_u8(b, 0); bb_u16(b, q_or_qmult ? q_or_qmult[k] : 0); bb_u32(b, qbase ? qbase[k] : 1); bb_u16(b, 0); bb_u16(b, 0); }
        }
    }
}
#endif
