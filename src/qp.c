/* SPDX-License-Identifier: Apache-2.0 */
#include "qp.h"
#include "rice.h"
#include <string.h>

static const uint32_t T[6] = { 40, 45, 51, 57, 64, 72 };

uint32_t crx_qstep(int32_t q)
{
    if (q < 0) return 0;
    int32_t e = q / 6, r = q % 6;
    if (e < 6) return T[r] >> (6 - e);
    if (e - 6 >= 32) return 0;
    return T[r] << (e - 6);
}

size_t crx_qmap_mem_size(uint32_t w, uint32_t h)
{
    uint32_t qw = (w + 7) / 8, qh = (h + 1) / 2, q4 = (h + 3) / 4, q8 = (h + 7) / 8;
    return (size_t)qw * (qh + qh + q4 + q8) + 2 * ((size_t)qw + 2);
}

static inline int32_t med(int32_t a, int32_t b, int32_t c)
{
    int32_t t = b - c;
    unsigned i = ((unsigned)((c < a) ^ (t < 0)) << 1) | (unsigned)((a < b) ^ (t < 0));
    switch (i) { case 0: case 1: return a + t; case 2: return a; default: return b; }
}
static inline int32_t abs32(int32_t v) { return v < 0 ? -v : v; }

bool crx_qmap_decode(const uint8_t *data, size_t len, uint32_t w, uint32_t h, unsigned levels, crx_qmap *qm, uint32_t *mem, uint32_t *overrun)
{
    uint32_t qw = (w + 7) / 8, qh = (h + 1) / 2, q4 = (h + 3) / 4, q8 = (h + 7) / 8;
    qm->qw = qw; qm->qh = qh;
    int32_t *qp = (int32_t *)mem;                        /* qh x qw decoded values + 4 */
    uint32_t *S1 = mem + (size_t)qw * qh, *S2 = S1 + (size_t)qw * qh, *S3 = S2 + (size_t)qw * q4;
    int32_t *lineA = (int32_t *)(S3 + (size_t)qw * q8) + 1, *lineB = lineA + qw + 2;
    crx_bits b; crx_bits_init(&b, data, len);
    unsigned k = 0;
    int32_t *p = lineA, *c = lineB;
    for (uint32_t y = 0; y < qh; y++) {
        if (y == 0) {
            int32_t left = 0;
            for (uint32_t x = 0; x < qw; x++) {
                uint32_t v = crx_code_qp(&b, k);
                c[x] = left + crx_signed(v); left = c[x];
                k = crx_adapt(k, v, 7);
            }
        } else {
            c[-1] = p[0];
            for (uint32_t x = 0; x < qw; x++) {
                int32_t a = c[(int32_t)x - 1], bb = p[x], cc = p[(int32_t)x - 1];
                uint32_t v = crx_code_qp(&b, k);
                c[x] = med(a, bb, cc) + crx_signed(v);
                if (x + 1 < qw) k = crx_adapt(k, (v + (uint32_t)abs32(2 * (p[x + 1] - p[x]))) >> 1, 7);
                else k = crx_adapt(k, v, 7);
            }
        }
        c[qw] = c[qw - 1] + 1;
        for (uint32_t x = 0; x < qw; x++) qp[(size_t)y * qw + x] = c[x] + 4;
        int32_t *t = p; p = c; c = t;
    }
    *overrun = crx_bits_overrun_bits(&b);
    /* Step tables per level (SPEC 7.3). Rows past the map repeat the last row. */
    for (uint32_t y = 0; y < qh; y++) for (uint32_t x = 0; x < qw; x++) {
        int32_t v = qp[(size_t)y * qw + x];
        if (v < 0) return false;
        S1[(size_t)y * qw + x] = crx_qstep(v);
    }
    qm->S[0] = S1; qm->rows[0] = qh;
    if (levels > 1) {
        for (uint32_t y = 0; y < q4; y++) for (uint32_t x = 0; x < qw; x++) {
            uint32_t r0 = 2 * y < qh ? 2 * y : qh - 1, r1 = 2 * y + 1 < qh ? 2 * y + 1 : qh - 1;
            int32_t v = (qp[(size_t)r0 * qw + x] + qp[(size_t)r1 * qw + x]) / 2;
            if (v < 0) return false;
            S2[(size_t)y * qw + x] = crx_qstep(v);
        }
        qm->S[1] = S2; qm->rows[1] = q4;
    }
    if (levels > 2) {
        for (uint32_t y = 0; y < q8; y++) for (uint32_t x = 0; x < qw; x++) {
            int32_t sum = 0;
            for (uint32_t j = 0; j < 4; j++) { uint32_t r = 4 * y + j < qh ? 4 * y + j : qh - 1; sum += qp[(size_t)r * qw + x]; }
            int32_t v = sum / 4;
            if (v < 0) return false;
            S3[(size_t)y * qw + x] = crx_qstep(v);
        }
        qm->S[2] = S3; qm->rows[2] = q8;
    }
    return true;
}
