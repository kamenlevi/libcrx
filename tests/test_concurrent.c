/* SPDX-License-Identifier: Apache-2.0 — several threads decoding at once, each with a pool, must all be exact. */
#include "crx.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const uint8_t *bytes; size_t len; unsigned threads; unsigned level; uint16_t *ref; size_t n; uint32_t w; int bad; } job;

static void *run(void *arg)
{
    job *j = arg; uint16_t *out = malloc(j->n * sizeof *out);
    for (int it = 0; it < 200 && !j->bad; it++) {
        crx_decoder *d; if (crx_open(j->bytes, j->len, &d) != CRX_OK) { j->bad = 1; break; }
        memset(out, 0xEE, j->n * sizeof *out);
        if (crx_decode(d, j->level, out, j->w, j->threads) != CRX_OK || memcmp(out, j->ref, j->n * sizeof *out)) j->bad = 1;
        crx_close(d);
    }
    free(out); return NULL;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)n); if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f); *len = (size_t)n; return p;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("concurrent: skipped (no seed files given)\n"); return 0; }
    int nfiles = argc - 1, bad = 0;
    job *jobs = calloc(nfiles * 2, sizeof *jobs); pthread_t *tids = calloc(nfiles * 2, sizeof *tids);
    for (int i = 0; i < nfiles; i++) {
        size_t len; uint8_t *bytes = read_file(argv[i + 1], &len);
        if (!bytes) { printf("cannot read %s\n", argv[i + 1]); return 1; }
        crx_decoder *d; if (crx_open(bytes, len, &d) != CRX_OK) { printf("cannot open %s\n", argv[i + 1]); return 1; }
        for (int k = 0; k < 2; k++) {
            job *j = &jobs[2 * i + k]; j->bytes = bytes; j->len = len; j->threads = k ? 8 : 3; j->level = (unsigned)k <= crx_get_info(d)->levels ? (unsigned)k : 0;
            j->n = crx_output_size(d, j->level, &j->w, NULL); j->ref = malloc(j->n * sizeof *j->ref);
            if (crx_decode(d, j->level, j->ref, j->w, 1) != CRX_OK) { printf("reference decode failed\n"); return 1; }
        }
        crx_close(d);
    }
    for (int i = 0; i < 2 * nfiles; i++) pthread_create(&tids[i], NULL, run, &jobs[i]);
    for (int i = 0; i < 2 * nfiles; i++) { pthread_join(tids[i], NULL); bad += jobs[i].bad; }
    printf(bad ? "concurrent: FAIL (%d jobs)\n" : "concurrent: ok (%d jobs x 200 decodes)\n", bad ? bad : 2 * nfiles);
    return bad != 0;
}
