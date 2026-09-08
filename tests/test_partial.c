/* SPDX-License-Identifier: Apache-2.0 — SPEC 10 on a real lossy file when one is available:
 * level-n raw output == reference analysis applied n times to the level-0 raw output. */
#include "../src/crx_internal.h"
#include "partial_check.h"
#include <stdio.h>
#include <string.h>

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)n); if (fread(p, 1, (size_t)n, f) != (size_t)n) { free(p); fclose(f); return NULL; }
    fclose(f); *len = (size_t)n; return p;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : getenv("CRX_LOSSY_SAMPLE");
    if (!path) { printf("partial: skipped (no lossy sample given)\n"); return 0; }
    size_t len; uint8_t *bytes = read_file(path, &len);
    if (!bytes) { printf("partial: skipped (cannot read %s)\n", path); return 0; }
    crx_decoder *d; if (crx_open(bytes, len, &d) != CRX_OK || d->levels == 0) { printf("partial: skipped (not a lossy file)\n"); return 0; }
    char msg[128] = "";
    int fails = partial_check(d, msg) != 0;
    if (fails) printf("%s\n", msg);
    crx_close(d); free(bytes);
    printf(fails ? "partial: FAIL\n" : "partial: ok (%s)\n", path);
    return fails != 0;
}
