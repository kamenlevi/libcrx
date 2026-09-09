/* SPDX-License-Identifier: Apache-2.0
 * A persistent pool that serves several concurrent runs at once: each
 * crx_pool_run registers its task list, workers (and the caller) pull tasks
 * from every active run in turn, and the caller returns when its own tasks
 * are done. Two decoders running at the same time therefore share the
 * machine at task granularity instead of taking turns. */
#include "pool.h"
#include <pthread.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <sched.h>

#define MAX_RUNS 32

typedef struct run_t {
    crx_task_fn fn; void *ctx;
    uint32_t n;
    atomic_uint next;         /* task handout, lock-free */
    atomic_uint done;
    atomic_int active;
    atomic_int takers;        /* workers currently inspecting this slot */
} run_t;

struct crx_pool {
    unsigned threads;             /* including the caller of each run */
    pthread_t *tids;
    pthread_mutex_t mu;           /* registry and sleeping only */
    pthread_cond_t cv_work;       /* a run was registered */
    pthread_cond_t cv_done;       /* a run completed or a slot was freed */
    run_t runs[MAX_RUNS];
    int quit;
};

/* Lock-free: take one task from any active run. Slot reuse is safe because a
 * run is retired only when no worker is inside this function for it. */
static int take(crx_pool *p, unsigned start, run_t **rp, uint32_t *idx)
{
    for (unsigned k = 0; k < MAX_RUNS; k++) {
        run_t *r = &p->runs[(start + k) % MAX_RUNS];
        if (!atomic_load(&r->active)) continue;
        atomic_fetch_add(&r->takers, 1);
        if (atomic_load(&r->active)) {
            uint32_t i = atomic_fetch_add(&r->next, 1);
            if (i < r->n) { *rp = r; *idx = i; atomic_fetch_sub(&r->takers, 1); return 1; }
        }
        atomic_fetch_sub(&r->takers, 1);
    }
    return 0;
}

static void finish(crx_pool *p, run_t *r)
{
    if (atomic_fetch_add(&r->done, 1) + 1 == r->n) {
        pthread_mutex_lock(&p->mu);
        pthread_cond_broadcast(&p->cv_done);
        pthread_mutex_unlock(&p->mu);
    }
}

static int any_work(crx_pool *p)
{
    for (unsigned k = 0; k < MAX_RUNS; k++)
        if (atomic_load(&p->runs[k].active) && atomic_load(&p->runs[k].next) < p->runs[k].n) return 1;
    return 0;
}

typedef struct { crx_pool *p; unsigned idx; } worker_arg;

static void *worker(void *arg)
{
    worker_arg *wa = arg; crx_pool *p = wa->p; unsigned idx = wa->idx; free(wa);
    for (;;) {
        run_t *r; uint32_t i;
        if (take(p, idx, &r, &i)) { r->fn(r->ctx, i, idx); finish(p, r); continue; }
        pthread_mutex_lock(&p->mu);
        while (!p->quit && !any_work(p)) pthread_cond_wait(&p->cv_work, &p->mu);
        int quit = p->quit;
        pthread_mutex_unlock(&p->mu);
        if (quit) return NULL;
    }
}

crx_pool *crx_pool_create(unsigned threads)
{
    if (threads < 1) threads = 1;
    crx_pool *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->threads = threads;
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv_work, NULL);
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
    pthread_mutex_lock(&p->mu); p->quit = 1; pthread_cond_broadcast(&p->cv_work); pthread_mutex_unlock(&p->mu);
    for (unsigned i = 0; i + 1 < p->threads; i++) pthread_join(p->tids[i], NULL);
    free(p->tids);
    pthread_mutex_destroy(&p->mu); pthread_cond_destroy(&p->cv_work); pthread_cond_destroy(&p->cv_done);
    free(p);
}

unsigned crx_pool_threads(const crx_pool *p) { return p ? p->threads : 1; }

void crx_pool_run(crx_pool *p, uint32_t n, crx_task_fn fn, void *ctx)
{
    if (n == 0) return;
    if (!p || p->threads == 1 || n == 1) { for (uint32_t i = 0; i < n; i++) fn(ctx, i, 0); return; }
    /* register under the mutex */
    pthread_mutex_lock(&p->mu);
    run_t *r = NULL;
    for (;;) {
        for (unsigned k = 0; k < MAX_RUNS; k++) if (!atomic_load(&p->runs[k].active) && atomic_load(&p->runs[k].takers) == 0) { r = &p->runs[k]; break; }
        if (r) break;
        pthread_cond_wait(&p->cv_done, &p->mu);
    }
    r->fn = fn; r->ctx = ctx; r->n = n;
    atomic_store(&r->next, 0); atomic_store(&r->done, 0);
    atomic_store(&r->active, 1);
    pthread_cond_broadcast(&p->cv_work);
    pthread_mutex_unlock(&p->mu);
    /* The caller works only on its own run (worker id 0 is unique per run). */
    for (;;) {
        uint32_t i = atomic_fetch_add(&r->next, 1);
        if (i >= n) break;
        fn(ctx, i, 0); finish(p, r);
    }
    pthread_mutex_lock(&p->mu);
    while (atomic_load(&r->done) < n) pthread_cond_wait(&p->cv_done, &p->mu);
    /* retire: no worker may be inside take() for this slot when it is reused */
    atomic_store(&r->active, 0);
    while (atomic_load(&r->takers) > 0) sched_yield();
    pthread_cond_broadcast(&p->cv_done);
    pthread_mutex_unlock(&p->mu);
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
        else if (p) {
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
    free(buf);
}
