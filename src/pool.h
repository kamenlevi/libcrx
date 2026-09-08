/* SPDX-License-Identifier: Apache-2.0
 * A small persistent thread pool: run `n` tasks with a barrier. The calling
 * thread works too. Portable pthreads. */
#ifndef CRX_POOL_H
#define CRX_POOL_H
#include <stddef.h>
#include <stdint.h>

typedef struct crx_pool crx_pool;
typedef void (*crx_task_fn)(void *ctx, uint32_t index, unsigned worker);

/* `threads` = total workers including the caller (1 = no pool). */
crx_pool *crx_pool_create(unsigned threads);
void      crx_pool_destroy(crx_pool *p);
unsigned  crx_pool_threads(const crx_pool *p);
/* Runs fn(ctx, i, worker) for i in [0, n) across the pool (worker in [0, threads));
 * returns when all are done. */
void      crx_pool_run(crx_pool *p, uint32_t n, crx_task_fn fn, void *ctx);

/* Process-wide pool: created on first use with `threads`, recreated when a
 * different count is asked for. Never destroyed (threads park on a condvar). */
crx_pool *crx_pool_shared(unsigned threads);

/* Workspace cache: large buffers reused across decodes to avoid page faults.
 * get() returns a buffer of at least `bytes`; put() hands it back. */
void *crx_ws_get(size_t bytes);
void  crx_ws_put(void *buf);
#endif
