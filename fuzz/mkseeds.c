/* SPDX-License-Identifier: Apache-2.0
 * Make tiny valid CR3 files from the spec (encoder in tests/enc.h, container
 * in tests/mkcr3.h) as fuzz seeds: a lossless one and a lossy v1 one whose
 * high-pass bands are empty. Decoding them must succeed; the lossless one
 * must reproduce its image exactly.
 *
 *   mkseeds <outdir> */
#include "../tests/enc.h"
#include "../tests/mkcr3.h"
#include "crx.h"
#include <stdio.h>

static uint32_t rnd(uint32_t *s) { *s = *s * 1103515245u + 12345u; return *s >> 8; }

static int write_file(const char *path, const bb *f)
{
    FILE *o = fopen(path, "wb"); if (!o) return 1;
    fwrite(f->p, 1, f->n, o); fclose(o); return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: mkseeds <outdir>\n"); return 2; }
    char path[1024]; uint32_t seed = 42; int bad = 0;
    /* ---- lossless: image 48x32 (planes 24x16), 4 planes, v1 ---- */
    {
        uint32_t W = 48, H = 32, pw = W / 2, ph = H / 2;
        int32_t *img = malloc(4 * pw * ph * sizeof *img);
        for (uint32_t i = 0; i < 4 * pw * ph; i++) img[i] = (int32_t)(rnd(&seed) % 3000) - 1500 + ((i % 7 == 0) ? 0 : 0);
        for (uint32_t i = 1; i < 4 * pw * ph; i++) if (rnd(&seed) % 3 == 0) img[i] = img[i - 1];   /* runs */
        enc_state st[4]; uint32_t sizes[4]; bb data = {0};
        for (int p = 0; p < 4; p++) {
            enc_init(&st[p], pw);
            for (uint32_t y = 0; y < ph; y++) enc_line_ll(&st[p], img + (size_t)p * pw * ph + y * pw);
            eb_flush(&st[p].e); sizes[p] = (uint32_t)st[p].e.n;
        }
        bb cs = {0};
        bb_codestream(&cs, 0x100, 0, 4, 1, sizes, NULL, NULL, 0, 0); bb_zeros(&cs, 4);
        uint32_t hdr = (uint32_t)cs.n;
        for (int p = 0; p < 4; p++) bb_put(&cs, st[p].e.p, st[p].e.n);
        mk_cmp1 c = { 0x100, W, H, W, H, 14, 4, 0, 0, 0, 0 };
        bb f = mk_file(&c, hdr, cs.p, (uint32_t)cs.n);
        snprintf(path, sizeof path, "%s/tiny-lossless.cr3", argv[1]); bad |= write_file(path, &f);
        /* self-check: decode and compare */
        crx_decoder *d; uint16_t *out = malloc(W * H * 2);
        if (crx_open(f.p, f.n, &d) != CRX_OK || crx_decode(d, 0, out, W, 1) != CRX_OK) { printf("tiny-lossless: decode failed\n"); bad = 1; }
        else {
            int mism = 0;
            for (uint32_t p = 0; p < 4; p++) for (uint32_t y = 0; y < ph; y++) for (uint32_t x = 0; x < pw; x++) {
                int32_t v = 8192 + img[(size_t)p * pw * ph + y * pw + x]; if (v < 0) v = 0; if (v > 16383) v = 16383;
                uint32_t r = 2 * y + (p >= 2), cc = 2 * x + (p & 1);
                if (out[r * W + cc] != v) mism++;
            }
            printf("tiny-lossless: %zu bytes, %s\n", f.n, mism ? "MISMATCH" : "round trip exact");
            if (mism) bad = 1; crx_close(d);
        }
        for (int p = 0; p < 4; p++) enc_free(&st[p]);
        free(img); free(out); free(cs.p); free(f.p); free(data.p);
    }
    /* ---- lossy v1: image 64x48, N=3, LL3 coded (base-band coder), nine empty high-pass bands ---- */
    {
        uint32_t W = 64, H = 48, pw = W / 2, ph = H / 2;
        uint32_t lw = (((pw + 1) / 2 + 1) / 2 + 1) / 2, lh = (((ph + 1) / 2 + 1) / 2 + 1) / 2;   /* 4 x 3 */
        enc_state st[4]; uint32_t sizes[40] = {0}; uint32_t q[10] = {4,4,4,4,16,16,22,26,26,32};
        int32_t *ll = malloc(lw * lh * sizeof *ll);
        for (int p = 0; p < 4; p++) {
            for (uint32_t i = 0; i < lw * lh; i++) ll[i] = (int32_t)(rnd(&seed) % 4000) - 2000;
            enc_init(&st[p], lw);
            for (uint32_t y = 0; y < lh; y++) enc_line_ll(&st[p], ll + y * lw);
            eb_flush(&st[p].e); sizes[p * 10] = (uint32_t)st[p].e.n;
        }
        bb cs = {0};
        bb_codestream(&cs, 0x100, 0, 4, 10, sizes, q, NULL, 0, 0); bb_zeros(&cs, 4);
        uint32_t hdr = (uint32_t)cs.n;
        for (int p = 0; p < 4; p++) bb_put(&cs, st[p].e.p, st[p].e.n);
        mk_cmp1 c = { 0x100, W, H, W, H, 14, 4, 0, 0, 3, 0 };
        bb f = mk_file(&c, hdr, cs.p, (uint32_t)cs.n);
        snprintf(path, sizeof path, "%s/tiny-lossy-v1.cr3", argv[1]); bad |= write_file(path, &f);
        crx_decoder *d; uint16_t *out = malloc(W * H * 2);
        for (unsigned L = 0; L <= 3; L++) {
            uint32_t w, h; if (crx_open(f.p, f.n, &d) != CRX_OK) { printf("tiny-lossy: open failed\n"); bad = 1; break; }
            crx_output_size(d, L, &w, &h);
            crx_status s = crx_decode(d, L, out, w, 1); crx_close(d);
            if (s != CRX_OK) { printf("tiny-lossy: level %u decode: %s\n", L, crx_strerror(s)); bad = 1; }
        }
        printf("tiny-lossy-v1: %zu bytes, levels 0..3 decode ok\n", f.n);
        for (int p = 0; p < 4; p++) enc_free(&st[p]);
        free(ll); free(out); free(cs.p); free(f.p);
    }
    return bad;
}
