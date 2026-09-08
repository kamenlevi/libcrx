/* SPDX-License-Identifier: Apache-2.0
 * SPEC 10 verifier. For every tile and plane: take the decoder's extended
 * level-(n-1) output (own samples, seam extras and, at level 0, the one
 * virtual sample beyond an even-sized seam edge), analyse it in the tile's
 * own frame with the reference transform (columns, then rows, mirrored at
 * the far end of the extension), and compare the own area with the
 * decoder's level-n output. Where the tile's width at level n-1 is odd the
 * last own low-pass column needs a high-pass coefficient that no sample can
 * give; there the stored extra coefficient of the HL band defines it, and
 * the verifier uses it explicitly (SPEC 10). Returns 0 or the failing level. */
#ifndef CRX_PARTIAL_CHECK_H
#define CRX_PARTIAL_CHECK_H
#include "../src/crx_internal.h"
#include "ref_wavelet.h"
#include <string.h>
#include <stdio.h>

static inline int partial_check(crx_decoder *d, char *msg)
{
    if (d->levels == 0) return 0;
    if (d->tiles_y != 1) { snprintf(msg, 128, "verifier handles one tile row"); return -1; }
    unsigned N = d->levels;
    for (uint32_t ti = 0; ti < d->tiles_x; ti++) for (uint32_t pi = 0; pi < d->nplanes; pi++) {
        const crx_tile *t = &d->tiles[ti];
        bool right = t->flags & CRX_RIGHT;
        int32_t *X; uint32_t ew, eh;
        if (crx_decode_plane_ext(d, 0, ti, pi, &X, &ew, &eh) != CRX_OK) { snprintf(msg, 128, "tile %u plane %u level 0 failed", ti, pi); return -1; }
        uint32_t own_w = t->w, own_h = t->h;                      /* own size at level n-1 */
        for (unsigned n = 1; n <= N; n++) {
            uint32_t nw = (ew + 1) / 2, nh = (eh + 1) / 2;
            int32_t *L = malloc((size_t)nw * nh * sizeof *L);
            /* column pass on the extended data, then row pass: ref_analyse_ll does exactly that, mirrored at the far end */
            ref_analyse_ll(X, ew, eh, ew, L, nw);
            uint32_t ow = (own_w + 1) / 2, oh = (own_h + 1) / 2;   /* own size at level n */
            if (right && (own_w & 1)) {
                /* last own column: l[i*] = A[2i*] + ((h[i*-1] + HLextra[i*] + 2) >> 2), with A the column-pass low rows */
                const crx_band *hl = &t->planes[pi].bands[3 * (N - n) + 1];
                int32_t *HL; if (crx_decode_band_ext(d, ti, pi, 3 * (N - n) + 1, &HL) != CRX_OK) { snprintf(msg, 128, "band decode failed"); free(L); free(X); return -1; }
                uint32_t istar = (own_w - 1) / 2;                  /* own H count = istar; the extra sits at column left_extra + istar */
                /* recompute the column pass low rows for the needed columns 2i*-2 .. 2i* (need h[i*-1] and A[2i*]) */
                int32_t *col = malloc((size_t)eh * sizeof *col), *cl = malloc((size_t)nh * sizeof *cl), *ch = malloc((size_t)(eh / 2 + 1) * sizeof *ch);
                int32_t *A = malloc((size_t)3 * nh * sizeof *A);   /* A[r*3 + j] = low row r, column 2i*-2+j */
                for (uint32_t j = 0; j < 3; j++) {
                    int32_t c = (int32_t)(2 * istar) - 2 + (int32_t)j;
                    if (c < 0) { for (uint32_t r = 0; r < nh; r++) A[r * 3 + j] = 0; continue; }
                    for (uint32_t r = 0; r < eh; r++) col[r] = X[(size_t)r * ew + c];
                    ref_analyse_line(col, eh, cl, ch);
                    for (uint32_t r = 0; r < nh; r++) A[r * 3 + j] = cl[r];
                }
                for (uint32_t r = 0; r < oh; r++) {
                    int32_t hprev;
                    if (istar == 0) hprev = HL[(size_t)r * hl->width + hl->left_extra];      /* mirror of h[0] = the extra itself */
                    else hprev = A[r * 3 + 1] - ((A[r * 3 + 0] + A[r * 3 + 2]) >> 1);        /* h[i*-1] from own samples */
                    int32_t hx = HL[(size_t)r * hl->width + hl->left_extra + istar];
                    L[(size_t)r * nw + istar] = A[r * 3 + 2] + ((hprev + hx + 2) >> 2);
                }
                free(col); free(cl); free(ch); free(A); free(HL);
            }
            if (t->flags & CRX_LEFT) {
                /* first own column: l[0] = A[0] + ((HLextra[-1] + h[0] + 2) >> 2), h[0] from own samples */
                const crx_band *hl = &t->planes[pi].bands[3 * (N - n) + 1];
                int32_t *HL; if (crx_decode_band_ext(d, ti, pi, 3 * (N - n) + 1, &HL) != CRX_OK) { snprintf(msg, 128, "band decode failed"); free(L); free(X); return -1; }
                int32_t *col = malloc((size_t)eh * sizeof *col), *cl = malloc((size_t)nh * sizeof *cl), *ch = malloc((size_t)(eh / 2 + 1) * sizeof *ch);
                int32_t *A = malloc((size_t)3 * nh * sizeof *A);
                for (uint32_t j = 0; j < 3; j++) {
                    uint32_t c = j < ew ? j : ew - 1;
                    for (uint32_t r = 0; r < eh; r++) col[r] = X[(size_t)r * ew + c];
                    ref_analyse_line(col, eh, cl, ch);
                    for (uint32_t r = 0; r < nh; r++) A[r * 3 + j] = cl[r];
                }
                for (uint32_t r = 0; r < oh; r++) {
                    int32_t h0 = A[r * 3 + 1] - ((A[r * 3 + 0] + A[r * 3 + 2]) >> 1);
                    int32_t hprev = HL[(size_t)r * hl->width];
                    L[(size_t)r * nw] = A[r * 3 + 0] + ((hprev + h0 + 2) >> 2);
                }
                free(col); free(cl); free(ch); free(A); free(HL);
            }
            int32_t *got = malloc((size_t)ow * oh * sizeof *got);
            if (crx_decode_plane_raw(d, n, ti, pi, got, ow) != CRX_OK) { snprintf(msg, 128, "tile %u plane %u level %u failed", ti, pi, n); free(got); free(L); free(X); return (int)n; }
            for (uint32_t y = 0; y < oh; y++)
                if (memcmp(got + (size_t)y * ow, L + (size_t)y * nw, ow * sizeof *got)) {
                    uint32_t x = 0; while (got[(size_t)y * ow + x] == L[(size_t)y * nw + x]) x++;
                    snprintf(msg, 128, "tile %u plane %u level %u: (%u,%u) ref %d got %d", ti, pi, n, x, y, L[(size_t)y * nw + x], got[(size_t)y * ow + x]);
                    free(got); free(L); free(X); return (int)n;
                }
            free(got); free(L); free(X);
            if (n < N && crx_decode_plane_ext(d, n, ti, pi, &X, &ew, &eh) != CRX_OK) { snprintf(msg, 128, "ext level %u failed", n); return (int)n; }
            own_w = ow; own_h = oh;
        }
    }
    return 0;
}
#endif
