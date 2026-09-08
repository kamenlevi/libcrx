/* SPDX-License-Identifier: Apache-2.0 — SPEC 7: step table and QP map round trip. */
#include "../src/qp.h"
#include "enc.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static uint32_t rnd(uint32_t *s) { *s = *s * 1103515245u + 12345u; return *s >> 8; }
static int32_t med3(int32_t a, int32_t b, int32_t c) { int32_t mx = a > b ? a : b, mn = a < b ? a : b; return c >= mx ? mn : c <= mn ? mx : a + b - c; }
static int32_t iabs2(int32_t v) { return v < 0 ? -v : v; }

int main(void)
{
    CHECK(crx_qstep(4) == 1 && crx_qstep(16) == 4 && crx_qstep(22) == 8 && crx_qstep(26) == 12 && crx_qstep(32) == 25);
    CHECK(crx_qstep(0) == 0 && crx_qstep(6) == 1 && crx_qstep(35) == 72 >> 1 && crx_qstep(36) == 40 && crx_qstep(-1) == 0);
    uint32_t seed = 11;
    for (int t = 0; t < 40 && fails < 5; t++) {
        uint32_t w = 1 + rnd(&seed) % 100, h = 1 + rnd(&seed) % 40;
        uint32_t qw = (w + 7) / 8, qh = (h + 1) / 2;
        int32_t *map = malloc(qw * qh * 4);
        for (uint32_t i = 0; i < qw * qh; i++) map[i] = (int32_t)(rnd(&seed) % 30) - 4 + (int32_t)(i % 3 == 0 ? 0 : 6);
        /* encode per SPEC 7.3 */
        ebits e = {0}; unsigned k = 0;
        int32_t *prev = calloc(qw + 2, 4), *cur = calloc(qw + 2, 4);
        for (uint32_t y = 0; y < qh; y++) {
            int32_t *p = prev + 1, *c = cur + 1;
            if (y == 0) {
                int32_t left = 0;
                for (uint32_t x = 0; x < qw; x++) { int32_t val = map[x]; uint32_t v = zigzag(val - left); enc_code(&e, v, k, 23, 8); k = crx_adapt(k, v, 7); c[x] = val; left = val; }
            } else {
                c[-1] = p[0];
                for (uint32_t x = 0; x < qw; x++) {
                    int32_t val = map[y * qw + x], pred = med3(c[(int32_t)x - 1], p[x], p[(int32_t)x - 1]);
                    uint32_t v = zigzag(val - pred); enc_code(&e, v, k, 23, 8);
                    if (x + 1 < qw) k = crx_adapt(k, (v + (uint32_t)iabs2(2 * (p[x + 1] - p[x]))) >> 1, 7); else k = crx_adapt(k, v, 7);
                    c[x] = val;
                }
            }
            c[qw] = c[qw - 1] + 1;
            int32_t *tmp = prev; prev = cur; cur = tmp;
        }
        eb_flush(&e);
        crx_qmap qm; uint32_t *mem = malloc(crx_qmap_mem_size(w, h) * 4); uint32_t over = 1;
        bool ok = crx_qmap_decode(e.p, e.n, w, h, 3, &qm, mem, &over);
        CHECK(ok && over == 0);
        int bad = 0;
        for (uint32_t y = 0; y < qh && ok; y++) for (uint32_t x = 0; x < qw; x++) if (qm.S[0][y * qw + x] != crx_qstep(map[y * qw + x] + 4)) bad++;
        for (uint32_t y = 0; y < (h + 3) / 4 && ok; y++) for (uint32_t x = 0; x < qw; x++) {
            uint32_t r0 = 2 * y < qh ? 2 * y : qh - 1, r1 = 2 * y + 1 < qh ? 2 * y + 1 : qh - 1;
            if (qm.S[1][y * qw + x] != crx_qstep((map[r0 * qw + x] + 4 + map[r1 * qw + x] + 4) / 2)) bad++;
        }
        CHECK(bad == 0);
        free(map); free(prev); free(cur); free(e.p); free(mem);
    }
    printf(fails ? "qp: FAIL (%d)\n" : "qp: ok\n", fails);
    return fails != 0;
}
