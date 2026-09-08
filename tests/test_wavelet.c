/* SPDX-License-Identifier: Apache-2.0 — SPEC 8: the example, and analysis(synthesis) = identity. */
#include "../src/wavelet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* Reference analysis (SPEC 8.1), one line, no seams: x[0..m) -> l, h. */
static void analyse_line(const int32_t *x, uint32_t m, int32_t *l, int32_t *h)
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

static uint32_t rnd(uint32_t *s) { *s = *s * 1103515245u + 12345u; return *s >> 8; }

int main(void)
{
    /* 8.1 worked example */
    int32_t l[3] = {10, 12, 11}, h[2] = {1, -2}, x[5];
    crx_synth_line(l, 3, h, 2, false, false, x, 5);
    CHECK(x[0] == 9 && x[1] == 11 && x[2] == 12 && x[3] == 10 && x[4] == 12);
    int32_t l2[3], h2[2]; analyse_line(x, 5, l2, h2);
    CHECK(l2[0] == 10 && l2[1] == 12 && l2[2] == 11 && h2[0] == 1 && h2[1] == -2);

    /* 1-D round trips, all lengths, random values including extremes */
    uint32_t seed = 3;
    for (uint32_t m = 1; m <= 70 && fails < 5; m++) for (int t = 0; t < 20; t++) {
        int32_t xs[70], ls[36], hs[36], y[70];
        for (uint32_t i = 0; i < m; i++) xs[i] = (int32_t)(rnd(&seed) % 40000) - 20000;
        analyse_line(xs, m, ls, hs);
        crx_synth_line(ls, (m + 1) / 2, hs, m / 2, false, false, y, m);
        CHECK(memcmp(xs, y, m * 4) == 0);
    }

    /* 2-D: analyse a random image (rows then columns), synthesise a stage, compare. */
    for (int t = 0; t < 30 && fails < 5; t++) {
        uint32_t W = 1 + rnd(&seed) % 41, Hh = 1 + rnd(&seed) % 29;
        uint32_t wl = (W + 1) / 2, wh = W / 2, hl = (Hh + 1) / 2, hh = Hh / 2;
        int32_t *img = malloc(W * Hh * 4), *rowL = malloc(wl * Hh * 4), *rowH = malloc((wh ? wh : 1) * Hh * 4);
        for (uint32_t i = 0; i < W * Hh; i++) img[i] = (int32_t)(rnd(&seed) % 16384) - 8192;
        /* The encoder analyses columns first, then rows (SPEC 8.2): columns of the image give
           a low half (hl rows) and a high half (hh rows); rows of each half give LL, HL and LH, HH. */
        int32_t *colL = rowL, *colH = rowH;                 /* reuse: colL is W x hl, colH is W x hh */
        colL = malloc(W * hl * 4); colH = malloc(W * (hh ? hh : 1) * 4);
        int32_t col[64], cl[32], ch[32];
        for (uint32_t c = 0; c < W; c++) {
            for (uint32_t r = 0; r < Hh; r++) col[r] = img[r * W + c];
            analyse_line(col, Hh, cl, ch);
            for (uint32_t r = 0; r < hl; r++) colL[r * W + c] = cl[r];
            for (uint32_t r = 0; r < hh; r++) colH[r * W + c] = ch[r];
        }
        int32_t *LL = malloc(wl * hl * 4), *LH = malloc(wl * (hh ? hh : 1) * 4), *HL = malloc((wh ? wh : 1) * hl * 4), *HH = malloc((wh ? wh : 1) * (hh ? hh : 1) * 4);
        for (uint32_t r = 0; r < hl; r++) analyse_line(colL + r * W, W, LL + r * wl, HL + r * (wh ? wh : 1));
        for (uint32_t r = 0; r < hh; r++) analyse_line(colH + r * W, W, LH + r * wl, HH + r * (wh ? wh : 1));
        free(colL); free(colH);
        int32_t *out = malloc(W * Hh * 4), *tmp = malloc(crx_stage_tmp_size(W, hl, hh) * 4);
        crx_synth_stage(LL, wl, hl, wl, HL, wh, hl, wh ? wh : 1, LH, wl, hh, wl, HH, wh, hh, wh ? wh : 1,
                        false, false, false, false, out, W, Hh, W, tmp);
        if (memcmp(img, out, W * Hh * 4)) { printf("2-D mismatch %ux%u\n", W, Hh); fails++; }
        free(img); free(rowL); free(rowH); free(LL); free(LH); free(HL); free(HH); free(out); free(tmp);
    }
    /* Seam: a 20-sample line split in two tiles of 10; the left tile carries the neighbour's
       coefficients as right extras, the right tile carries one left extra. Both halves must equal the whole. */
    {
        int32_t xs[20], ls[10], hs[10], y[20];
        for (uint32_t i = 0; i < 20; i++) xs[i] = (int32_t)(rnd(&seed) % 1000);
        analyse_line(xs, 20, ls, hs);
        /* left tile: m = 10 even, right: needs l[0..5] (6 = 5+1) and h[0..5] (6 = 5+1) per SPEC 4.2 */
        crx_synth_line(ls, 6, hs, 6, false, true, y, 10);
        CHECK(memcmp(xs, y, 10 * 4) == 0);
        /* right tile: samples 10..19; l[5..9], h with extra h[4] in front then h[5..9] */
        crx_synth_line(ls + 5, 5, hs + 4, 6, true, false, y + 10, 10);
        CHECK(memcmp(xs + 10, y + 10, 10 * 4) == 0);
    }
    printf(fails ? "wavelet: FAIL (%d)\n" : "wavelet: ok\n", fails);
    return fails != 0;
}
