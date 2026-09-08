/* SPDX-License-Identifier: Apache-2.0 */
#include "../tools/sha256.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int check(const char *what, const void *d, size_t n, const char *want)
{
    char hex[65]; sha256_hex(d, n, hex);
    if (strcmp(hex, want)) { printf("sha256(%s) = %s, want %s\n", what, hex, want); return 1; }
    return 0;
}

int main(void)
{
    int bad = 0;
    bad += check("empty", "", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    bad += check("abc", "abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char *s2 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    bad += check("448-bit", s2, strlen(s2), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    char *m = malloc(1000000); memset(m, 'a', 1000000);
    bad += check("million a", m, 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    /* streaming in odd pieces must equal one shot */
    sha256_ctx c; uint8_t d[32]; sha256_init(&c);
    for (size_t i = 0; i < 1000000; i += 777) sha256_update(&c, m + i, i + 777 <= 1000000 ? 777 : 1000000 - i);
    sha256_final(&c, d);
    char hex[65]; for (int i = 0; i < 32; i++) sprintf(hex + 2*i, "%02x", d[i]);
    if (strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")) { printf("streaming mismatch\n"); bad++; }
    free(m);
    printf(bad ? "sha256: FAIL\n" : "sha256: ok\n");
    return bad;
}
