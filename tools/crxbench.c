/* SPDX-License-Identifier: Apache-2.0
 *
 * crxbench: time libcrx decodes. Files are read into memory first, so the
 * numbers are decode only.
 *
 *   crxbench [-l level] [-t threads] [-n reps] [-v] <file.cr3>...
 *
 * Per file: the median of n reps. Overall: median, p90 and worst of the
 * per-file medians, and throughput in megapixels per second. */
#include "common.h"
#include "crx.h"

int main(int argc, char **argv)
{
    unsigned level = 0, threads = 1; int reps = 5, verbose = 0, a = 1;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-l") && a + 1 < argc) level = (unsigned)atoi(argv[++a]);
        else if (!strcmp(argv[a], "-t") && a + 1 < argc) threads = (unsigned)atoi(argv[++a]);
        else if (!strcmp(argv[a], "-n") && a + 1 < argc) reps = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-v")) verbose = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[a]); return 2; }
    }
    if (a >= argc) { fprintf(stderr, "usage: crxbench [-l level] [-t threads] [-n reps] [-v] <file.cr3>...\n"); return 2; }
    int nfiles = argc - a;
    double *med = malloc(nfiles * sizeof *med), *times = malloc(reps * sizeof *times);
    double mpix_total = 0, ms_total = 0; int done = 0, skipped = 0;
    uint16_t *buf = NULL; size_t bufcap = 0;
    for (int i = 0; i < nfiles; i++) {
        size_t len; uint8_t *bytes = read_file(argv[a + i], &len);
        if (!bytes) { skipped++; continue; }
        int ok = 1; uint32_t w = 0, h = 0;
        for (int r = 0; r < reps && ok; r++) {
            double t = now_ms();
            crx_decoder *d = NULL;
            if (crx_open(bytes, len, &d) != CRX_OK) { ok = 0; break; }
            size_t n = crx_output_size(d, level, &w, &h);
            if (!n) { ok = 0; crx_close(d); break; }
            if (n > bufcap) { bufcap = n; buf = realloc(buf, n * sizeof *buf); }
            if (crx_decode(d, level, buf, w, threads) != CRX_OK) ok = 0;
            crx_close(d);
            times[r] = now_ms() - t;
        }
        free(bytes);
        if (!ok) { skipped++; if (verbose) printf("skip %s\n", argv[a + i]); continue; }
        qsort(times, reps, sizeof *times, cmp_double);
        med[done] = times[reps / 2];
        mpix_total += (double)w * h / 1e6; ms_total += med[done];
        if (verbose) printf("%8.2f ms  %ux%u  %s\n", med[done], w, h, argv[a + i]);
        done++;
    }
    if (!done) { printf("crxbench: nothing decoded (%d files skipped)\n", skipped); return 1; }
    qsort(med, done, sizeof *med, cmp_double);
    printf("crxbench: level %u, %u thread%s, %d files (%d skipped): median %.2f ms, p90 %.2f ms, worst %.2f ms, %.0f Mpix/s, libcrx %s\n",
           level, threads, threads == 1 ? "" : "s", done, skipped,
           med[done / 2], med[(done * 9) / 10 < done ? (done * 9) / 10 : done - 1], med[done - 1],
           mpix_total / (ms_total / 1e3), crx_version());
    return 0;
}
