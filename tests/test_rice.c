/* SPDX-License-Identifier: Apache-2.0 — SPEC 5: worked examples and round trips. */
#include "../src/lines.h"
#include "enc.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void from_bits(const char *s, ebits *e) { free(e->p); memset(e, 0, sizeof *e); for (; *s; s++) if (*s == '0' || *s == '1') eb_bit(e, *s - '0'); eb_flush(e); }

static uint32_t rnd(uint32_t *st) { *st = *st * 1103515245u + 12345u; return *st >> 8; }

int main(void)
{
    ebits e = {0}; crx_bits b;
    /* 5.2 examples */
    from_bits("1", &e); crx_bits_init(&b, e.p, e.n); CHECK(crx_code(&b, 0) == 0);
    from_bits("001 10", &e); crx_bits_init(&b, e.p, e.n); CHECK(crx_code(&b, 2) == 10);
    from_bits("0000001 0", &e); crx_bits_init(&b, e.p, e.n); CHECK(crx_code(&b, 1) == 12);
    { char s[80]; memset(s, '0', 41); s[41] = '1'; strcpy(s + 42, "101010101010101010101"); from_bits(s, &e); crx_bits_init(&b, e.p, e.n); CHECK(crx_code(&b, 3) == 0x155555); }
    /* 5.3 */
    CHECK(crx_signed(0) == 0 && crx_signed(1) == -1 && crx_signed(2) == 1 && crx_signed(3) == -2 && crx_signed(4) == 2);
    for (int32_t v = -70000; v < 70000; v += 7) CHECK(crx_signed(zigzag(v)) == v);
    /* 5.4 */
    CHECK(crx_adapt(2, 10, 15) == 2 && crx_adapt(1, 12, 15) == 3 && crx_adapt(0, 0, 15) == 0 && crx_adapt(15, 1 << 20, 15) == 15 && crx_adapt(15, 1 << 20, 0) == 17);
    /* 5.5 */
    { unsigned s = 0; from_bits("1 1 1 0", &e); crx_bits_init(&b, e.p, e.n); CHECK(crx_run(&b, &s, 10) == 3 && s == 1); }
    { unsigned s = 8; from_bits("1 1 0 11", &e); crx_bits_init(&b, e.p, e.n); CHECK(crx_run(&b, &s, 10) == 8 && s == 8); }
    { unsigned s = 0; from_bits("0", &e); crx_bits_init(&b, e.p, e.n); CHECK(crx_run(&b, &s, 10) == 0 && s == 0); }
    /* a one bit at the very bottom of a full 64-bit window: 63 zeros then 1, then 21 bits */
    { uint8_t z[11] = {0}; z[7] = 1; z[8] = 0xAB; z[9] = 0xCD; z[10] = 0xE0; crx_bits_init(&b, z, 11); CHECK(crx_code(&b, 5) == 0x1579BC); }
    /* zeros() across many bytes and past the end */
    { uint8_t z[10] = {0}; z[9] = 1; crx_bits_init(&b, z, 10); CHECK(crx_bits_zeros(&b) == 79); }
    { uint8_t z[2] = {0}; crx_bits_init(&b, z, 2); uint32_t n = crx_bits_zeros(&b); CHECK(n >= 16 && crx_bits_overrun_bits(&b) > 0); }
    /* 5.6 worked example: width 4 first line, bits 1 1 0 | 0 1 | 1 | 0 0 1 ... encoder produces the reference string for [0,0,0,-1] */
    { enc_state st; enc_init(&st, 4); int32_t v[4] = {0, 0, 0, -1}; enc_line_ll(&st, v); eb_flush(&st.e);
      CHECK(st.e.n == 1 && st.e.p[0] == 0xE4);     /* run of 3: 1 1 1 0 ; then -1 at k=0: 0 1 ; padded: 1110 0100 */
      crx_linestate d; int32_t mem[3 * 6]; crx_line_init(&d, st.e.p, st.e.n, 4, mem);
      const int32_t *out = crx_line_ll(&d); CHECK(out && out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == -1);
      enc_free(&st); }
    /* Round trips: random images with flat runs, both decoders, many widths. */
    uint32_t seed = 7;
    for (int trial = 0; trial < 60 && fails < 5; trial++) {
        uint32_t w = 1 + rnd(&seed) % 40, h = 1 + rnd(&seed) % 12;
        int32_t *img = malloc((size_t)w * h * 4);
        int hf = trial & 1;
        for (uint32_t i = 0; i < w * h; i++) {
            uint32_t r = rnd(&seed);
            int32_t v = (r % 5 == 0) ? 0 : (int32_t)(r % 41) - 20;
            if (r % 7 == 0 && i) v = img[i - 1];                          /* runs */
            if (r % 97 == 0) v = (int32_t)(r % 200000) - 100000;          /* escapes */
            if (hf && r % 3 == 0) v = 0;
            img[i] = v;
        }
        enc_state st; enc_init(&st, w);
        for (uint32_t y = 0; y < h; y++) { if (hf) enc_line_hf(&st, img + y * w); else enc_line_ll(&st, img + y * w); }
        eb_flush(&st.e);
        crx_linestate d; int32_t *mem = malloc(3 * (w + 2) * 4); crx_line_init(&d, st.e.p, st.e.n, w, mem);
        for (uint32_t y = 0; y < h; y++) {
            const int32_t *out = hf ? crx_line_hf(&d) : crx_line_ll(&d);
            CHECK(out != NULL);
            if (out) for (uint32_t x = 0; x < w; x++) if (out[x] != img[y * w + x]) { if (fails < 5) printf("trial %d (%s) w=%u h=%u mismatch at (%u,%u): %d vs %d\n", trial, hf ? "hf" : "ll", w, h, x, y, out[x], img[y*w+x]); fails++; break; }
        }
        CHECK(crx_bits_overrun_bits(&d.bits) == 0);
        free(img); free(mem); enc_free(&st);
    }
    free(e.p);
    printf(fails ? "rice: FAIL (%d)\n" : "rice: ok\n", fails);
    return fails != 0;
}
