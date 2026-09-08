/* SPDX-License-Identifier: Apache-2.0 */
#include "crx.h"
#include <stdlib.h>

#define CRX_VERSION "0.0.0"

struct crx_decoder {
    const uint8_t *bytes;
    size_t len;
    crx_info info;
};

crx_status crx_open(const void *bytes, size_t len, crx_decoder **out)
{
    if (!bytes || !out) return CRX_E_ARG;
    *out = NULL;
    (void)len;
    /* Milestone 2 puts the container walk and header parse here. */
    return CRX_E_UNSUPPORTED;
}

const crx_info *crx_get_info(const crx_decoder *d)
{
    return d ? &d->info : NULL;
}

size_t crx_output_size(const crx_decoder *d, unsigned level, uint32_t *w, uint32_t *h)
{
    if (!d || level > d->info.levels) return 0;
    uint32_t ww = d->info.width >> level, hh = d->info.height >> level;
    if (w) *w = ww;
    if (h) *h = hh;
    return (size_t)ww * hh;
}

crx_status crx_decode(crx_decoder *d, unsigned level, uint16_t *dst, size_t stride, unsigned threads)
{
    if (!d || !dst) return CRX_E_ARG;
    (void)level; (void)stride; (void)threads;
    return CRX_E_UNSUPPORTED;
}

void crx_close(crx_decoder *d)
{
    free(d);
}

const char *crx_strerror(crx_status s)
{
    switch (s) {
    case CRX_OK:            return "ok";
    case CRX_E_ARG:         return "bad argument";
    case CRX_E_FORMAT:      return "not a CR3 file";
    case CRX_E_UNSUPPORTED: return "unsupported variant";
    case CRX_E_CORRUPT:     return "corrupt data";
    case CRX_E_TRUNCATED:   return "truncated data";
    case CRX_E_NOMEM:       return "out of memory";
    }
    return "unknown status";
}

const char *crx_version(void) { return CRX_VERSION; }
