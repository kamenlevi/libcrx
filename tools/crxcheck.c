/* SPDX-License-Identifier: Apache-2.0
 *
 * crxcheck: decode files with libcrx and compare, bit for bit, with the
 * oracle fingerprints written by crxoracle.
 *
 *   crxcheck [-v] [-t threads] -o oracle.tsv <file.cr3>...
 *
 * Prints one line per failure (every file with -v) and one summary line.
 * Exit 0 only when every file with an oracle row decoded exactly. */
#include "common.h"
#include "crx.h"

typedef struct { char sha[65]; char model[64]; uint32_t rw, rh; char pix[65]; } row_t;
typedef struct { char *path; char sha[65]; } man_t;
static man_t *man; static size_t nman, capman;

static int load_manifest(const char *path)
{
    FILE *f = fopen(path, "r"); if (!f) return -1;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *tab = strchr(line, '\t'); if (!tab || tab - line != 64) continue;
        char *tab2 = strchr(tab + 1, '\t'); if (!tab2) continue;
        char *nl = strchr(tab2, '\n'); if (nl) *nl = 0;
        if (nman == capman) { capman = capman ? capman * 2 : 1024; man = realloc(man, capman * sizeof *man); }
        memcpy(man[nman].sha, line, 64); man[nman].sha[64] = 0; man[nman].path = strdup(tab2 + 1); nman++;
    }
    fclose(f); return 0;
}
static int cmp_man(const void *a, const void *b) { return strcmp(((const man_t *)a)->path, ((const man_t *)b)->path); }
static const char *manifest_sha(const char *path)
{
    man_t key = { (char *)path, "" };
    man_t *m = bsearch(&key, man, nman, sizeof *man, cmp_man);
    return m ? m->sha : NULL;
}

static row_t *rows; static size_t nrows, caprows;

static int load_oracle(const char *path)
{
    FILE *f = fopen(path, "r"); if (!f) return -1;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        if (nrows == caprows) { caprows = caprows ? caprows * 2 : 1024; rows = realloc(rows, caprows * sizeof *rows); }
        row_t *r = &rows[nrows];
        char *tok = strtok(line, "\t"); int col = 0; int ok = 0;
        while (tok) {
            switch (col) {
            case 0: strncpy(r->sha, tok, 64); r->sha[64] = 0; break;
            case 1: strncpy(r->model, tok, 63); r->model[63] = 0; break;
            case 2: r->rw = (uint32_t)strtoul(tok, NULL, 10); break;
            case 3: r->rh = (uint32_t)strtoul(tok, NULL, 10); break;
            case 15: strncpy(r->pix, tok, 64); r->pix[64] = 0; ok = 1; break;
            }
            tok = strtok(NULL, "\t"); col++;
        }
        if (ok) nrows++;
    }
    fclose(f);
    return 0;
}

static int cmp_row(const void *a, const void *b) { return strcmp(((const row_t *)a)->sha, ((const row_t *)b)->sha); }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *oracle = NULL; int verbose = 0, headers_only = 0; unsigned threads = 1; int a = 1;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-v")) verbose = 1;
        else if (!strcmp(argv[a], "-H")) headers_only = 1;
        else if (!strcmp(argv[a], "-m") && a + 1 < argc) { if (load_manifest(argv[++a]) < 0) { fprintf(stderr, "cannot read manifest\n"); return 2; } }
        else if (!strcmp(argv[a], "-o") && a + 1 < argc) oracle = argv[++a];
        else if (!strcmp(argv[a], "-t") && a + 1 < argc) threads = (unsigned)atoi(argv[++a]);
        else { fprintf(stderr, "unknown option %s\n", argv[a]); return 2; }
    }
    if (!oracle || a >= argc) { fprintf(stderr, "usage: crxcheck [-v] [-H] [-t threads] [-m manifest.tsv] -o oracle.tsv <file.cr3>...\n"); return 2; }
    if (load_oracle(oracle) < 0) { fprintf(stderr, "cannot read %s\n", oracle); return 2; }
    qsort(rows, nrows, sizeof *rows, cmp_row);
    if (nman) qsort(man, nman, sizeof *man, cmp_man);

    size_t exact = 0, fail = 0, unsupported = 0, nooracle = 0, total = 0;
    uint16_t *buf = NULL; size_t bufcap = 0;
    double t0 = now_ms();
    for (; a < argc; a++) {
        total++;
        size_t len; uint8_t *bytes = read_file(argv[a], &len);
        if (!bytes) { printf("FAIL unreadable %s\n", argv[a]); fail++; continue; }
        row_t key; const char *msha = nman ? manifest_sha(argv[a]) : NULL;
        if (msha) memcpy(key.sha, msha, 65); else sha256_hex(bytes, len, key.sha);
        row_t *r = bsearch(&key, rows, nrows, sizeof *rows, cmp_row);
        if (!r) { if (verbose) printf("no-oracle %s\n", argv[a]); nooracle++; free(bytes); continue; }

        crx_decoder *d = NULL;
        crx_status s = crx_open(bytes, len, &d);
        if (s == CRX_E_UNSUPPORTED) { if (verbose) printf("unsupported %s %s\n", r->model, argv[a]); unsupported++; free(bytes); continue; }
        if (s != CRX_OK) { printf("FAIL open:%s %s %s\n", crx_strerror(s), r->model, argv[a]); fail++; free(bytes); continue; }

        uint32_t w, h; size_t n = crx_output_size(d, 0, &w, &h);
        if (w != r->rw || h != r->rh) {
            printf("FAIL geometry ours %ux%u oracle %ux%u %s %s\n", w, h, r->rw, r->rh, r->model, argv[a]);
            fail++; crx_close(d); free(bytes); continue;
        }
        if (headers_only) { exact++; if (verbose) printf("headers-ok %s %s\n", r->model, argv[a]); crx_close(d); free(bytes); continue; }
        if (n > bufcap) { bufcap = n; buf = realloc(buf, n * sizeof *buf); }
        s = crx_decode(d, 0, buf, w, threads);
        if (s == CRX_E_UNSUPPORTED) { if (verbose) printf("unsupported %s %s\n", r->model, argv[a]); unsupported++; crx_close(d); free(bytes); continue; }
        if (s != CRX_OK) { printf("FAIL decode:%s %s %s\n", crx_strerror(s), r->model, argv[a]); fail++; crx_close(d); free(bytes); continue; }
        char ph[65]; sha256_u16le(buf, n, ph);
        if (strcmp(ph, r->pix) == 0) { exact++; if (verbose) printf("exact %s %s\n", r->model, argv[a]); }
        else { printf("FAIL pixels %s %s\n", r->model, argv[a]); fail++; }
        crx_close(d); free(bytes);
    }
    double dt = (now_ms() - t0) / 1e3;
    size_t judged = total - nooracle;
    printf("crxcheck%s: %zu %s of %zu (%zu unsupported, %zu FAIL, %zu without oracle) in %.1f s, libcrx %s\n",
           headers_only ? " -H" : "", exact, headers_only ? "headers-ok" : "exact", judged, unsupported, fail, nooracle, dt, crx_version());
    return (fail == 0 && unsupported == 0 && judged > 0) ? 0 : 1;
}
