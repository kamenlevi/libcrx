/* SPDX-License-Identifier: Apache-2.0 */
#include "pool.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdatomic.h>

struct crx_pool {
    unsigned threads;             /* including the caller */
    pthread_t *tids;              /* threads - 1 workers */
    pthread_mutex_t mu, run_mu;           /* run_mu: one run at a time; concurrent decoders queue */
    pthread_cond_t cv_start, cv_done;
    uint64_t generation;          /* bumped per run */
    unsigned running;             /* workers still inside the current run */
    int quit;
    /* current run */
    crx_task_fn fn; void *ctx; uint32_t n; atomic_uint next;
};

static void work(crx_pool *p, unsigned worker)
{
    for (;;) {
        uint32_t i = atomic_fetch_add(&p->next, 1);
        if (i >= p->n) return;
        p->fn(p->ctx, i, worker);
    }
}
typedef struct { crx_pool *p; unsigned idx; } worker_arg;

static void *worker(void *arg)
{
    worker_arg *wa = arg; crx_pool *p = wa->p; unsigned idx = wa->idx; free(wa);
    uint64_t seen = 0;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (p->generation == seen && !p->quit) pthread_cond_wait(&p->cv_start, &p->mu);
        if (p->quit) { pthread_mutex_unlock(&p->mu); return NULL; }
        seen = p->generation;
        pthread_mutex_unlock(&p->mu);
        work(p, idx);
        pthread_mutex_lock(&p->mu);
        if (--p->running == 0) pthread_cond_signal(&p->cv_done);
        pthread_mutex_unlock(&p->mu);
    }
}

crx_pool *crx_pool_create(unsigned threads)
{
    if (threads < 1) threads = 1;
    crx_pool *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->threads = threads;
    pthread_mutex_init(&p->mu, NULL);
    pthread_mutex_init(&p->run_mu, NULL);
    pthread_cond_init(&p->cv_start, NULL);
    pthread_cond_init(&p->cv_done, NULL);
    if (threads > 1) {
        p->tids = calloc(threads - 1, sizeof *p->tids);
        for (unsigned i = 0; i < threads - 1; i++) {
            worker_arg *wa = malloc(sizeof *wa); wa->p = p; wa->idx = i + 1;
            if (pthread_create(&p->tids[i], NULL, worker, wa)) { free(wa); p->threads = i + 1; break; }
        }
    }
    return p;
}

void crx_pool_destroy(crx_pool *p)
{
    if (!p) return;
    pthread_mutex_lock(&p->mu); p->quit = 1; pthread_cond_broadcast(&p->cv_start); pthread_mutex_unlock(&p->mu);
    for (unsigned i = 0; i + 1 < p->threads; i++) pthread_join(p->tids[i], NULL);
    free(p->tids);
    pthread_mutex_destroy(&p->mu); pthread_mutex_destroy(&p->run_mu); pthread_cond_destroy(&p->cv_start); pthread_cond_destroy(&p->cv_done);
    free(p);
}

unsigned crx_pool_threads(const crx_pool *p) { return p ? p->threads : 1; }

void crx_pool_run(crx_pool *p, uint32_t n, crx_task_fn fn, void *ctx)
{
    if (n == 0) return;
    if (!p || p->threads == 1 || n == 1) { for (uint32_t i = 0; i < n; i++) fn(ctx, i, 0); return; }
    pthread_mutex_lock(&p->run_mu);
    pthread_mutex_lock(&p->mu);
    p->fn = fn; p->ctx = ctx; p->n = n; atomic_store(&p->next, 0);
    p->running = p->threads - 1; p->generation++;
    pthread_cond_broadcast(&p->cv_start);
    pthread_mutex_unlock(&p->mu);
    work(p, 0);
    pthread_mutex_lock(&p->mu);
    while (p->running) pthread_cond_wait(&p->cv_done, &p->mu);
    pthread_mutex_unlock(&p->mu);
    pthread_mutex_unlock(&p->run_mu);
}

/* ---- shared pools: one per requested thread count, never destroyed ---- */
#define POOL_SLOTS 4
static pthread_mutex_t shared_mu = PTHREAD_MUTEX_INITIALIZER;
static crx_pool *shared_pools[POOL_SLOTS];

crx_pool *crx_pool_shared(unsigned threads)
{
    if (threads < 1) threads = 1;
    pthread_mutex_lock(&shared_mu);
    crx_pool *p = NULL; int free_slot = -1;
    for (int i = 0; i < POOL_SLOTS; i++) {
        if (shared_pools[i] && shared_pools[i]->threads == threads) { p = shared_pools[i]; break; }
        if (!shared_pools[i] && free_slot < 0) free_slot = i;
    }
    if (!p) {
        p = crx_pool_create(threads);
        if (p && free_slot >= 0) shared_pools[free_slot] = p;
        else if (p) { /* more than POOL_SLOTS distinct counts: fall back to the largest existing pool */
            crx_pool_destroy(p); p = shared_pools[0];
            for (int i = 1; i < POOL_SLOTS; i++) if (shared_pools[i]->threads > p->threads) p = shared_pools[i];
        }
    }
    pthread_mutex_unlock(&shared_mu);
    return p;
}

/* ---- workspace cache ---- */
#define WS_SLOTS 16
typedef struct { void *buf; size_t bytes; int in_use; } ws_slot;
static ws_slot ws[WS_SLOTS];
static pthread_mutex_t ws_mu = PTHREAD_MUTEX_INITIALIZER;

void *crx_ws_get(size_t bytes)
{
    pthread_mutex_lock(&ws_mu);
    int best = -1;
    for (int i = 0; i < WS_SLOTS; i++)
        if (!ws[i].in_use && ws[i].buf && ws[i].bytes >= bytes && (best < 0 || ws[i].bytes < ws[best].bytes)) best = i;
    if (best >= 0) { ws[best].in_use = 1; void *b = ws[best].buf; pthread_mutex_unlock(&ws_mu); return b; }
    /* none fits: take a free slot (dropping a too-small buffer if needed) */
    int slot = -1;
    for (int i = 0; i < WS_SLOTS; i++) if (!ws[i].in_use && !ws[i].buf) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < WS_SLOTS; i++) if (!ws[i].in_use) { slot = i; break; }
    void *b = malloc(bytes);
    if (slot >= 0 && b) { free(ws[slot].buf); ws[slot].buf = b; ws[slot].bytes = bytes; ws[slot].in_use = 1; }
    pthread_mutex_unlock(&ws_mu);
    return b;
}

void crx_ws_put(void *buf)
{
    if (!buf) return;
    pthread_mutex_lock(&ws_mu);
    for (int i = 0; i < WS_SLOTS; i++) if (ws[i].buf == buf) { ws[i].in_use = 0; pthread_mutex_unlock(&ws_mu); return; }
    pthread_mutex_unlock(&ws_mu);
    free(buf);                            /* was not cached (all slots busy) */
}
