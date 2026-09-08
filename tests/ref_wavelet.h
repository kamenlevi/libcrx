/* SPDX-License-Identifier: Apache-2.0
 * Reference integer 5/3 analysis written from SPEC 8.1 and 8.2 (columns,
 * then rows). Slow and obvious on purpose: it defines what a partial decode
 * must equal. Shared by tests and by crxcheck -p. */
#ifndef CRX_REF_WAVELET_H
#define CRX_REF_WAVELET_H
#include <stdint.h>
#include <stdlib.h>

static inline void ref_analyse_line(const int32_t *x, uint32_t m, int32_t *l, int32_t *h)
{
    uint32_t nh = m / 2, nl = (m + 1) / 2;
    for (uint32_t i = 0; i < nh; i++) {
        int32_t right = (2 * i + 2 < m) ? x[2 * i + 2] : x[2 * i];
        h[i] = x[2 * i + 1] - ((x[2 * i] + right) >> 1);
    }
    for (uint32_t i = 0; i < nl; i++) {
        int32_t hm = (i == 0) ? (nh ? h[0] : 0) : h[i - 1];
        int32_t hp = (i < nh) ? h[i] : (nh ? h[nh - 1] : 0);
        l[i] = x[2 * i] + ((hm + hp + 2) >> 2);
    }
}

/* One level of analysis of a W x H image (stride sx); writes the LL band
 * (ceil2(W) x ceil2(H), stride sll). Uses malloc; fine for a reference. */
static inline void ref_analyse_ll(const int32_t *x, uint32_t W, uint32_t H, size_t sx, int32_t *ll, size_t sll)
{
    uint32_t hl = (H + 1) / 2, wl = (W + 1) / 2;
    int32_t *colL = malloc((size_t)W * hl * sizeof *colL);
    int32_t *col = malloc((size_t)H * sizeof *col), *cl = malloc((size_t)hl * sizeof *cl), *ch = malloc((size_t)(H / 2 + 1) * sizeof *ch);
    for (uint32_t c = 0; c < W; c++) {
        for (uint32_t r = 0; r < H; r++) col[r] = x[(size_t)r * sx + c];
        ref_analyse_line(col, H, cl, ch);
        for (uint32_t r = 0; r < hl; r++) colL[(size_t)r * W + c] = cl[r];
    }
    int32_t *rh = malloc((size_t)(W / 2 + 1) * sizeof *rh);
    for (uint32_t r = 0; r < hl; r++) ref_analyse_line(colL + (size_t)r * W, W, ll + (size_t)r * sll, rh);
    free(colL); free(col); free(cl); free(ch); free(rh);
    (void)wl;
}
#endif
