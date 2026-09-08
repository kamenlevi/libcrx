/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CRX_SHA256_H
#define CRX_SHA256_H
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t used; } sha256_ctx;
void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t n);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256_hex(const void *data, size_t n, char hex[65]);
#endif
