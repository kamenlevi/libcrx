/* SPDX-License-Identifier: Apache-2.0 */
#include "crx.h"
#include <stdio.h>
#include <string.h>

int main(void)
{
    int bad = 0;
    crx_decoder *d = (crx_decoder *)1;
    if (crx_open(NULL, 0, &d) != CRX_E_ARG) { puts("open(NULL) must be E_ARG"); bad++; }
    if (crx_open("x", 1, NULL) != CRX_E_ARG) { puts("open(out=NULL) must be E_ARG"); bad++; }
    d = (crx_decoder *)1;
    crx_status s = crx_open("not a cr3 file at all", 21, &d);
    if (s == CRX_OK) { puts("garbage must not open"); bad++; }
    if (d != NULL) { puts("failed open must leave *out NULL"); bad++; }
    if (crx_get_info(NULL) != NULL) { puts("get_info(NULL) must be NULL"); bad++; }
    if (crx_output_size(NULL, 0, NULL, NULL) != 0) { puts("output_size(NULL) must be 0"); bad++; }
    if (crx_decode(NULL, 0, NULL, 0, 0) != CRX_E_ARG) { puts("decode(NULL) must be E_ARG"); bad++; }
    crx_close(NULL);
    for (int i = 0; i <= CRX_E_NOMEM; i++) if (!strcmp(crx_strerror((crx_status)i), "unknown status")) { printf("status %d has no text\n", i); bad++; }
    if (!crx_version() || !*crx_version()) { puts("version empty"); bad++; }
    printf(bad ? "api: FAIL\n" : "api: ok\n");
    return bad;
}
