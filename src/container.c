/* SPDX-License-Identifier: Apache-2.0
 * ISO base media walk: find the CRX track with the largest image and return
 * its sample location and CMP1 box. SPEC section 1. Every access is bounded. */
#include "crx_internal.h"
#include <string.h>

typedef struct { size_t start, payload, end; char type[5]; } box_t;

/* Parse the box at `at` inside [at, limit). */
static bool box_at(const uint8_t *buf, size_t at, size_t limit, box_t *b)
{
    if (limit < at || limit - at < 8) return false;
    uint64_t size = crx_rd32(buf + at);
    size_t hdr = 8;
    if (size == 1) {
        if (limit - at < 16) return false;
        size = crx_rd64(buf + at + 8); hdr = 16;
    } else if (size == 0) {
        size = limit - at;
    }
    if (size < hdr || size > limit - at) return false;
    memcpy(b->type, buf + at + 4, 4); b->type[4] = 0;
    b->start = at; b->payload = at + hdr; b->end = at + (size_t)size;
    return true;
}

/* First child box of a given type inside [from, limit). */
static bool find_child(const uint8_t *buf, size_t from, size_t limit, const char *type, box_t *out)
{
    size_t p = from; box_t b;
    while (p < limit && box_at(buf, p, limit, &b)) {
        if (!strcmp(b.type, type)) { *out = b; return true; }
        if (b.end <= p) return false;
        p = b.end;
    }
    return false;
}

static bool track_sample(const uint8_t *buf, const box_t *stbl, uint64_t *off, uint64_t *size)
{
    box_t b;
    if (!find_child(buf, stbl->payload, stbl->end, "stsz", &b) || b.end - b.payload < 12) return false;
    uint32_t sample_size = crx_rd32(buf + b.payload + 4);
    uint32_t count = crx_rd32(buf + b.payload + 8);
    if (count < 1) return false;
    if (sample_size == 0) {
        if (b.end - b.payload < 16) return false;
        sample_size = crx_rd32(buf + b.payload + 12);
    }
    *size = sample_size;
    if (find_child(buf, stbl->payload, stbl->end, "co64", &b)) {
        if (b.end - b.payload < 16 || crx_rd32(buf + b.payload + 4) < 1) return false;
        *off = crx_rd64(buf + b.payload + 8);
        return true;
    }
    if (find_child(buf, stbl->payload, stbl->end, "stco", &b)) {
        if (b.end - b.payload < 12 || crx_rd32(buf + b.payload + 4) < 1) return false;
        *off = crx_rd32(buf + b.payload + 8);
        return true;
    }
    return false;
}

crx_status crx_find_image_track(const uint8_t *buf, size_t len, int want_smallest, uint64_t *sample_off, uint64_t *sample_size,
                                const uint8_t **cmp1, size_t *cmp1_len, const uint8_t **iad1, size_t *iad1_len, uint32_t *track)
{
    box_t ftyp, moov, trak, mdia, minf, stbl, stsd, entry, c, cdi1, iad;
    *iad1 = NULL; *iad1_len = 0;
    if (!find_child(buf, 0, len, "ftyp", &ftyp)) return CRX_E_FORMAT;
    if (ftyp.end - ftyp.payload < 4 || memcmp(buf + ftyp.payload, "crx ", 4)) return CRX_E_FORMAT;
    if (!find_child(buf, 0, len, "moov", &moov)) return CRX_E_FORMAT;

    uint64_t best_area = 0; bool found = false;
    size_t p = moov.payload; uint32_t index = 0;
    while (p < moov.end && box_at(buf, p, moov.end, &trak)) {
        if (!strcmp(trak.type, "trak")) {
            if (find_child(buf, trak.payload, trak.end, "mdia", &mdia) &&
                find_child(buf, mdia.payload, mdia.end, "minf", &minf) &&
                find_child(buf, minf.payload, minf.end, "stbl", &stbl) &&
                find_child(buf, stbl.payload, stbl.end, "stsd", &stsd) &&
                stsd.end - stsd.payload >= 8 &&
                box_at(buf, stsd.payload + 8, stsd.end, &entry) && !strcmp(entry.type, "CRAW") &&
                entry.end - entry.payload >= 82 &&
                find_child(buf, entry.payload + 82, entry.end, "CMP1", &c) &&
                c.end - c.payload >= 52) {
                uint64_t w = crx_rd32(buf + c.payload + 8), h = crx_rd32(buf + c.payload + 12);
                uint64_t off, size;
                bool better = found ? (want_smallest ? w * h < best_area : w * h > best_area) : true;
                if (better && track_sample(buf, &stbl, &off, &size)) {
                    best_area = w * h; found = true;
                    *sample_off = off; *sample_size = size;
                    *cmp1 = buf + c.payload; *cmp1_len = c.end - c.payload; *track = index;
                    *iad1 = NULL; *iad1_len = 0;
                    if (find_child(buf, entry.payload + 82, entry.end, "CDI1", &cdi1) && cdi1.end - cdi1.payload >= 4 &&
                        find_child(buf, cdi1.payload + 4, cdi1.end, "IAD1", &iad) && iad.end - iad.payload >= 4) {
                        *iad1 = buf + iad.payload + 4; *iad1_len = iad.end - iad.payload - 4;
                    }
                }
            }
            index++;
        }
        if (trak.end <= p) break;
        p = trak.end;
    }
    if (!found) return CRX_E_FORMAT;
    if (*sample_off > len || *sample_size > len - *sample_off) return CRX_E_TRUNCATED;
    return CRX_OK;
}
