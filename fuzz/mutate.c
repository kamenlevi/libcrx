/* SPDX-License-Identifier: Apache-2.0
 *
 * Mutation fuzzer: takes seed files, applies random byte/bit/block mutations
 * and truncations, and runs crx_open + crx_decode on the result. Meant to
 * run under -fsanitize=address,undefined for hours; any crash is a bug.
 *
 *   mutate [-n iterations] [-s seed] [-l level] <seed files...>
 *
 * Prints a progress line every 1000 iterations and the status histogram at
 * the end. Never prints per-iteration output. */
#include "crx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static uint64_t rng;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 16); }

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)n + 1);
    if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f); *len = (size_t)n; return p;
}

int main(int argc, char **argv)
{
    long iterations = 100000; unsigned level = 0; rng = (uint64_t)time(NULL) * 2654435761u | 1; int a = 1;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-n")) iterations = atol(argv[++a]);
        else if (!strcmp(argv[a], "-s")) rng = (uint64_t)atoll(argv[++a]) | 1;
        else if (!strcmp(argv[a], "-l")) level = (unsigned)atoi(argv[++a]);
    }
    int nseeds = argc - a; if (nseeds < 1) { fprintf(stderr, "usage: mutate [-n iters] [-s seed] [-l level] <files>\n"); return 2; }
    uint8_t **seeds = calloc(nseeds, sizeof *seeds); size_t *lens = calloc(nseeds, sizeof *lens);
    for (int i = 0; i < nseeds; i++) { seeds[i] = read_file(argv[a + i], &lens[i]); if (!seeds[i]) { fprintf(stderr, "cannot read %s\n", argv[a + i]); return 2; } }
    long hist[8] = {0}; long decoded = 0; double slow_limit = 5.0; int slow_count = 0;
    const char *slow_dir = getenv("MUTATE_SLOW_DIR");
    uint16_t *out = NULL; size_t outcap = 0;
    double t0 = (double)clock() / CLOCKS_PER_SEC;
    for (long it = 0; it < iterations; it++) {
        int si = (int)(rnd() % (unsigned)nseeds);
        size_t len = lens[si]; uint8_t *buf = malloc(len + 16); memcpy(buf, seeds[si], len);
        /* Mutations: mostly in the first 64 KB (headers) or in the codestream headers area, sometimes anywhere. */
        unsigned nmut = 1 + rnd() % 8;
        for (unsigned m = 0; m < nmut; m++) {
            size_t pos = (rnd() % 4 == 0) ? rnd() % len : rnd() % (len < 65536 ? len : 65536);
            switch (rnd() % 6) {
            case 0: buf[pos] = (uint8_t)rnd(); break;
            case 1: buf[pos] ^= (uint8_t)(1u << (rnd() % 8)); break;
            case 2: buf[pos] = 0xFF; break;
            case 3: buf[pos] = 0; break;
            case 4: { size_t n = rnd() % 64; for (size_t i = 0; i < n && pos + i < len; i++) buf[pos + i] = (uint8_t)rnd(); } break;
            case 5: { uint32_t v = rnd() % 4 == 0 ? 0xFFFFFFFFu : rnd() % 0x10000; if (pos + 4 <= len) { buf[pos] = (uint8_t)(v >> 24); buf[pos+1] = (uint8_t)(v >> 16); buf[pos+2] = (uint8_t)(v >> 8); buf[pos+3] = (uint8_t)v; } } break;
            }
        }
        if (rnd() % 8 == 0) len = rnd() % len;                       /* truncation */
        crx_decoder *d = NULL;
        double it0 = (double)clock() / CLOCKS_PER_SEC;
        crx_status s = crx_open(buf, len, &d);
        hist[s < 8 ? s : 7]++;
        if (s == CRX_OK) {
            uint32_t w, h; size_t n = crx_output_size(d, level, &w, &h);
            if (n && n <= (size_t)1 << 27) {
                if (n > outcap) { outcap = n; out = realloc(out, n * sizeof *out); }
                memset(out, 0xAB, n * sizeof *out);
                crx_status r = crx_decode(d, level, out, w, 1);
                if (r == CRX_OK) decoded++;
            }
            crx_close(d);
        }
        double dt = (double)clock() / CLOCKS_PER_SEC - it0;
        if (dt > slow_limit) {
            const crx_info *inf = NULL; crx_decoder *dd = NULL;
            if (crx_open(buf, len, &dd) == CRX_OK) inf = crx_get_info(dd);
            printf("SLOW %.1f s at iteration %ld seed %d len %zu%s", dt, it, si, len, inf ? "" : "\n");
            if (inf) printf(" image %ux%u levels %u tiles %ux%u\n", inf->width, inf->height, inf->levels, inf->tiles_x, inf->tiles_y);
            if (dd) crx_close(dd);
            if (slow_dir && slow_count < 20) {
                char path[1024]; snprintf(path, sizeof path, "%s/slow-%ld.cr3", slow_dir, it);
                FILE *f = fopen(path, "wb"); if (f) { fwrite(buf, 1, len, f); fclose(f); printf("  saved %s\n", path); }
                slow_count++;
            }
            fflush(stdout);
        }
        free(buf);
        if ((it + 1) % 1000 == 0) { printf("%ld iterations, %ld decoded, %.0f s\n", it + 1, decoded, (double)clock() / CLOCKS_PER_SEC - t0); fflush(stdout); }
    }
    printf("done: %ld iterations, %ld decoded; open results: ok %ld arg %ld format %ld unsupported %ld corrupt %ld truncated %ld nomem %ld\n",
           iterations, decoded, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6]);
    return 0;
}
