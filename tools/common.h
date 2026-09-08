/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CRX_TOOLS_COMMON_H
#define CRX_TOOLS_COMMON_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "sha256.h"

static inline uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    uint8_t *p = malloc((size_t)n + 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f); *len = (size_t)n; return p;
}

static inline double now_ms(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* Hash a uint16 buffer as little-endian bytes, whatever the host order. */
static inline void sha256_u16le(const uint16_t *p, size_t n, char hex[65])
{
    const uint16_t one = 1;
    if (*(const uint8_t *)&one == 1) { sha256_hex(p, n * 2, hex); return; }
    sha256_ctx c; uint8_t d[32]; sha256_init(&c);
    uint8_t buf[8192];
    while (n) {
        size_t k = n < 4096 ? n : 4096;
        for (size_t i = 0; i < k; i++) { buf[2*i] = (uint8_t)p[i]; buf[2*i+1] = (uint8_t)(p[i] >> 8); }
        sha256_update(&c, buf, 2*k); p += k; n -= k;
    }
    sha256_final(&c, d);
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { hex[2*i] = hx[d[i] >> 4]; hex[2*i+1] = hx[d[i] & 15]; }
    hex[64] = 0;
}

static inline int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
#endif
