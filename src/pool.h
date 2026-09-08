/* SPDX-License-Identifier: Apache-2.0
 * A small persistent thread pool: run `n` tasks with a barrier. The calling
 * thread works too. Portable pthreads. */
#ifndef CRX_POOL_H
#define CRX_POOL_H
#include <stddef.h>
#include <stdint.h>

typedef struct crx_pool crx_pool;
typedef void (*crx_task_fn)(void *ctx, uint32_t index);

/* `threads` = total workers including the caller (1 = no pool). */
crx_pool *crx_pool_create(unsigned threads);
void      crx_pool_destroy(crx_pool *p);
unsigned  crx_pool_threads(const crx_pool *p);
/* Runs fn(ctx, i) for i in [0, n) across the pool; returns when all are done. */
void      crx_pool_run(crx_pool *p, uint32_t n, crx_task_fn fn, void *ctx);
#endif
