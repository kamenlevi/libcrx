/* SPDX-License-Identifier: Apache-2.0 — SPEC 4.2: the seam-extras rule. */
#include "../src/crx_internal.h"
#include <stdio.h>

/* Values observed from the reference decoder's behaviour for every (levels,
 * width mod 8): per level, H extra then L extra. Facts about the format. */
static const uint8_t observed[3][8][6] = {
  {{1,1,0,0,0,0},{1,0,0,0,0,0},{1,1,0,0,0,0},{1,0,0,0,0,0},{1,1,0,0,0,0},{1,0,0,0,0,0},{1,1,0,0,0,0},{1,0,0,0,0,0}},
  {{1,1,1,1,0,0},{1,0,1,0,0,0},{1,2,2,1,0,0},{1,1,1,1,0,0},{1,1,1,1,0,0},{1,0,1,0,0,0},{1,2,2,1,0,0},{1,1,1,1,0,0}},
  {{1,1,1,1,1,1},{1,0,1,0,1,0},{1,2,2,2,2,1},{1,1,1,2,2,1},{1,1,1,2,2,1},{1,0,1,1,1,1},{1,2,2,1,1,1},{1,1,1,1,1,1}} };

int main(void)
{
    int fails = 0;
    for (unsigned N = 1; N <= 3; N++)
        for (uint32_t w = 22; w < 4000; w++) {
            uint8_t eh[3] = {0}, el[3] = {0};
            crx_seam_extras(N, w, eh, el);
            for (unsigned l = 0; l < N; l++)
                if (eh[l] != observed[N-1][w % 8][2*l] || el[l] != observed[N-1][w % 8][2*l+1]) {
                    if (fails < 5) printf("N=%u w=%u level %u: got H+%u L+%u, observed H+%u L+%u\n", N, w, l, eh[l], el[l], observed[N-1][w%8][2*l], observed[N-1][w%8][2*l+1]);
                    fails++;
                }
        }
    /* The SPEC's worked example: N = 2, w = 10. */
    uint8_t eh[3], el[3]; crx_seam_extras(2, 10, eh, el);
    if (!(eh[0] == 1 && el[0] == 2 && eh[1] == 2 && el[1] == 1)) { printf("worked example wrong\n"); fails++; }
    if (crx_ceil2n(3094, 1) != 1547 || crx_ceil2n(1722, 3) != 216 || crx_ceil2n(5, 2) != 2) { printf("ceil2n wrong\n"); fails++; }
    printf(fails ? "band_geometry: FAIL (%d)\n" : "band_geometry: ok\n", fails);
    return fails != 0;
}
