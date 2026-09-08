/* SPDX-License-Identifier: Apache-2.0 — SPEC 1, 2, 3 on a synthetic file. */
#include "../src/crx_internal.h"
#include "mkcr3.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    /* v1 lossless, one tile, 4 planes, one band each. Plane is 20x12 (image 40x24). */
    mk_cmp1 c = { 0x100, 40, 24, 40, 24, 14, 4, 0, 0, 0, 0 };
    uint32_t sizes[4] = { 100, 200, 300, 400 };
    bb cs = {0};
    bb_codestream(&cs, 0x100, 0, 4, 1, sizes, NULL, NULL, 0, 0);
    bb_zeros(&cs, 4);
    uint32_t hdr = (uint32_t)cs.n;
    bb_zeros(&cs, 1008);                       /* the data, with 8 bytes of slack */
    bb f = mk_file(&c, hdr, cs.p, (uint32_t)cs.n);

    crx_decoder *d = NULL;
    crx_status s = crx_open(f.p, f.n, &d);
    CHECK(s == CRX_OK);
    if (s == CRX_OK) {
        const crx_info *i = crx_get_info(d);
        CHECK(i->width == 40 && i->height == 24 && i->planes == 4 && i->levels == 0 && i->tiles_x == 1);
        CHECK(i->cfa[0] == 0 && i->cfa[1] == 1 && i->cfa[2] == 1 && i->cfa[3] == 2);
        CHECK(d->hdr_size == hdr && d->sample_size == cs.n);
        CHECK(d->tiles[0].w == 20 && d->tiles[0].h == 12 && d->tiles[0].flags == 0);
        CHECK(d->tiles[0].size == 1000);
        CHECK(d->tiles[0].planes[2].bands[0].data_off == d->sample_off + hdr + 300);
        CHECK(d->tiles[0].planes[3].bands[0].coded_size == 400 && d->tiles[0].planes[3].bands[0].data_size == 400);
        CHECK(d->tiles[0].planes[0].bands[0].width == 20 && d->tiles[0].planes[0].bands[0].height == 12);
        uint32_t w, h; CHECK(crx_output_size(d, 0, &w, &h) == 40 * 24 && w == 40 && h == 24);
        CHECK(crx_output_size(d, 1, &w, &h) == 0);
        crx_close(d);
    }

    /* Corruptions must be refused, never crash. */
    CHECK(crx_open(f.p, 10, &d) == CRX_E_FORMAT && d == NULL);
    for (size_t cut = 0; cut < f.n; cut += 7) { crx_status r = crx_open(f.p, cut, &d); CHECK(r != CRX_OK || cut >= f.n); if (r == CRX_OK) crx_close(d); }
    {   /* plane sizes that do not add up */
        bb g = {0}; bb_put(&g, f.p, f.n);
        size_t tile_hdr = f.n - cs.n;                    /* mdat payload start */
        g.p[tile_hdr + 4 + 3] += 1;                      /* tile size + 1 */
        CHECK(crx_open(g.p, g.n, &d) == CRX_E_CORRUPT);
        free(g.p);
    }
    {   /* v2 with a QP table and extra bytes, two levels of q metadata */
        mk_cmp1 c2 = { 0x200, 40, 24, 40, 24, 14, 4, 0, 0, 3, 0 };
        uint32_t sz2[40]; uint32_t qm[10] = {0,0,0,0,4,4,8,8,8,16}, qb[10] = {1,1,1,1,0,0,0,0,0,0};
        for (int k = 0; k < 40; k++) sz2[k] = (uint32_t)(10 + k);
        bb cs2 = {0};
        bb_codestream(&cs2, 0x200, 0, 4, 10, sz2, qm, qb, 77, 5);
        bb_zeros(&cs2, 4);
        uint32_t hdr2 = (uint32_t)cs2.n;
        uint32_t data = 77 + 5; for (int k = 0; k < 40; k++) data += sz2[k];
        bb_zeros(&cs2, data);
        bb f2 = mk_file(&c2, hdr2, cs2.p, (uint32_t)cs2.n);
        s = crx_open(f2.p, f2.n, &d);
        CHECK(s == CRX_OK);
        if (s == CRX_OK) {
            CHECK(d->tiles[0].qp_size == 77 && d->tiles[0].extra == 5);
            CHECK(d->tiles[0].planes[0].data_off == d->sample_off + hdr2 + 82);
            CHECK(d->tiles[0].planes[0].bands[9].qmult == 16 && d->tiles[0].planes[0].bands[0].qbase == 1);
            CHECK(d->tiles[0].planes[1].bands[0].data_off == d->sample_off + hdr2 + 82 + (10+11+12+13+14+15+16+17+18+19));
            uint32_t w, h; CHECK(crx_output_size(d, 3, &w, &h) == 6 * 4 && w == 6 && h == 4);   /* plane 20x12 -> 10x6 -> 5x3 -> 3x2 */
            crx_close(d);
        }
        free(cs2.p); free(f2.p);
    }
    free(cs.p); free(f.p);
    printf(fails ? "container: FAIL\n" : "container: ok\n");
    return fails;
}
