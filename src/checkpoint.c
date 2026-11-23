#include "crc32c.h"
#include "store.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Checkpoints make a committed image durable. Swapping dirty lists lets
 * writers keep committing while a checkpoint writes the older image.
 *
 * The checkpoint registers at its target LSN to keep old user versions
 * alive across I/O. Commit copies any bitmap the checkpoint can see.
 * Dirty frames stay resident. Backend calls, log hooks and logging run
 * with only ckpt_mu held.
 */

/* ---- registry ------------------------------------------------------------ */

void ps_reg_add(ps_store *s, ps_snapref *r)
{
    PS_ASSERT(s->reg_tail == NULL || s->reg_tail->lsn <= r->lsn);
    r->prev = s->reg_tail;
    r->next = NULL;
    if (s->reg_tail != NULL) {
        s->reg_tail->next = r;
    } else {
        s->reg_head = r;
    }
    s->reg_tail = r;
}

void ps_reg_remove(ps_store *s, ps_snapref *r)
{
    if (r->prev != NULL) {
        r->prev->next = r->next;
    } else {
        PS_ASSERT(s->reg_head == r);
        s->reg_head = r->next;
    }
    if (r->next != NULL) {
        r->next->prev = r->prev;
    } else {
        PS_ASSERT(s->reg_tail == r);
        s->reg_tail = r->prev;
    }
    r->prev = NULL;
    r->next = NULL;
}

u64 ps_reg_min(const ps_store *s)
{
    return s->reg_head != NULL ? s->reg_head->lsn : UINT64_MAX;
}

bool ps_reg_any_at_or_above(const ps_store *s, u64 c)
{
    return s->reg_tail != NULL && s->reg_tail->lsn >= c;
}

/* ---- failed state and log hooks ------------------------------------------- */

void ps_store_fail(ps_store *s, int err)
{
    const bool was = atomic_exchange_explicit(&s->failed, true, memory_order_acq_rel);
    if (was) {
        return;
    }
    /*
     * If a waiter saw the old flag under mem_mu, this broadcast still
     * reaches it after it starts waiting.
     */
    pthread_mutex_lock(&s->mem_mu);
    pthread_cond_broadcast(&s->ckpt_wait_cv);
    pthread_cond_signal(&s->ckpt_thread_cv);
    pthread_mutex_unlock(&s->mem_mu);
    char eb[128];
    ps_log(
        s,
        PS_LOG_ERROR,
        "store failed: %s (error %d); reopen to recover",
        ps_strerror(err, eb, sizeof(eb)),
        err);
}

void ps_set_log_hooks(ps_store *s, const ps_log_hooks *hooks)
{
    pthread_mutex_lock(&s->ckpt_mu);
    if (hooks != NULL) {
        s->hooks = *hooks;
    } else {
        memset(&s->hooks, 0, sizeof(s->hooks));
    }
    pthread_mutex_unlock(&s->ckpt_mu);
}

/* ---- checkpoint attempts -------------------------------------------------- */

/* Monotonic time in milliseconds for the retry and periodic timers. */
static u64 now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64) ts.tv_sec * 1000u + (u64) ts.tv_nsec / 1000000u;
}

#define PS_BACKOFF_MIN_MS 100u
#define PS_BACKOFF_MAX_MS 10000u

/*
 * ckpt_mu serializes attempts. Publish the counter and result under
 * mem_mu so a waiter can tell when its requested attempt has finished.
 */
static int ckpt_attempt(ps_store *s, bool bg)
{
    pthread_mutex_lock(&s->ckpt_mu);
    pthread_mutex_lock(&s->mem_mu);
    s->ckpt_started++;
    s->ckpt_inflight = true;
    pthread_mutex_unlock(&s->mem_mu);

    s->ckpt_bg = bg;
    const int rv = ps_checkpoint_run(s);
    s->ckpt_bg = false;

    pthread_mutex_lock(&s->mem_mu);
    s->ckpt_done = s->ckpt_started;
    s->ckpt_inflight = false;
    s->ckpt_last_err = rv;
    const u64 now = now_ms();
    s->ckpt_last_end_ms = now;
    if (rv == 0) {
        s->ckpt_backoff_ms = 0;
        s->ckpt_retry_at_ms = 0;
    } else {
        s->ckpt_backoff_ms = s->ckpt_backoff_ms == 0 ?
            PS_BACKOFF_MIN_MS :
            PS_MIN(s->ckpt_backoff_ms * 2u, PS_BACKOFF_MAX_MS);
        s->ckpt_retry_at_ms = now + s->ckpt_backoff_ms;
    }
    if (s->ckpt_waiters > 0) {
        pthread_cond_broadcast(&s->ckpt_wait_cv);
    }
    pthread_mutex_unlock(&s->mem_mu);
    pthread_mutex_unlock(&s->ckpt_mu);
    return rv;
}

int ps_checkpoint(ps_store *s)
{
    return ckpt_attempt(s, false);
}

/* ---- checkpoint ---------------------------------------------------------- */

/*
 * Hold reg_mu. Commit released its pins on these old bitmap versions, so
 * the checkpoint was their last reader.
 */
static void free_bitmap_copies(ps_store *s)
{
    while (s->bmcopy_head != NULL) {
        ps_vnode *v = s->bmcopy_head;
        s->bmcopy_head = v->retire_next;
        v->retire_next = NULL;
        ps_entry *e = v->entry;
        const ps_page_id id = e->id;
        ps_latch(s->pt, id);
        PS_ASSERT(e->head != v);
        ps_node_free(s, e, v);
        ps_unlatch(s->pt, id);
    }
}

/*
 * Set the next dirty bit before clearing the checkpoint bit. A momentary
 * clean entry could be released. Hold reg_mu throughout the merge.
 */
static void merge_back(ps_store *s)
{
    const u32 c = s->ckpt_idx;
    const u32 n = s->next_idx;
    const u8 cbit = (u8) (1u << c);
    const u8 nbit = (u8) (1u << n);
    ps_entry *e = s->dirty_head[c];
    while (e != NULL) {
        ps_entry *next = e->dirty_next[c];
        e->dirty_next[c] = NULL;
        const u8 was = atomic_fetch_or_explicit(&e->dirty, nbit, memory_order_relaxed);
        if ((was & nbit) == 0) {
            e->dirty_next[n] = s->dirty_head[n];
            s->dirty_head[n] = e;
        }
        atomic_fetch_and_explicit(&e->dirty, (u8) ~cbit, memory_order_relaxed);
        e = next;
    }
    s->dirty_head[c] = NULL;
}

/*
 * began means ckpt_begin succeeded. Abort the backend only for a retryable
 * error; a fatal error leaves the backend failed.
 */
static int ckpt_failed(ps_store *s, u64 L, int err, bool fatal, bool began)
{
    if (began && !fatal) {
        s->be.ckpt_abort(s->be.ctx);
    }
    if (fatal) {
        ps_store_fail(s, err);
    }
    pthread_mutex_lock(&s->reg_mu);
    if (!fatal) {
        merge_back(s);
    }
    /* Retain the dirty list after a fatal failure for teardown at close. */
    free_bitmap_copies(s);
    ps_reg_remove(s, &s->ckpt_ref);
    s->ckpt_running = false;
    ps_retire_process(s);
    pthread_mutex_unlock(&s->reg_mu);
    atomic_fetch_add_explicit(&s->checkpoints_failed, 1, memory_order_relaxed);
    /* A background failure has no caller to see it, so log it at ERROR. */
    char eb[128];
    ps_log(
        s,
        fatal || s->ckpt_bg ? PS_LOG_ERROR : PS_LOG_WARN,
        "%scheckpoint at LSN %llu failed: %s (error %d)%s",
        s->ckpt_bg ? "background " : "",
        (unsigned long long) L,
        ps_strerror(err, eb, sizeof(eb)),
        err,
        fatal          ? "; the store has failed" :
            s->ckpt_bg ? "; will retry" :
                         "");
    return fatal ? EIO : err;
}

static int item_cmp(const void *a, const void *b)
{
    const u32 x = ((const ps_ckpt_item *) a)->e->id.raw;
    const u32 y = ((const ps_ckpt_item *) b)->e->id.raw;
    return (x > y) - (x < y);
}

/* Copy the checkpoint's private dirty list into an array sorted by page ID. */
static int collect(ps_store *s, u32 c, u32 *count)
{
    u32 n = 0;
    for (const ps_entry *e = s->dirty_head[c]; e != NULL; e = e->dirty_next[c]) {
        n++;
    }
    if (n > s->ckpt_cap) {
        ps_ckpt_item *p = realloc(s->ckpt_items, (usize) n * sizeof(*p));
        if (p == NULL) {
            return ENOMEM;
        }
        s->ckpt_items = p;
        s->ckpt_cap = n;
        atomic_store_explicit(
            &s->ckpt_bytes,
            (u64) n * sizeof(*p) + s->page_size,
            memory_order_relaxed);
    }
    u32 i = 0;
    for (ps_entry *e = s->dirty_head[c]; e != NULL; e = e->dirty_next[c]) {
        s->ckpt_items[i].e = e;
        s->ckpt_items[i].v = NULL;
        s->ckpt_items[i].data = false;
        i++;
    }
    if (n > 1) {
        qsort(s->ckpt_items, n, sizeof(*s->ckpt_items), item_cmp);
    }
    *count = n;
    return 0;
}

/* Keep free markers for later cleanup, but count only data writes. */
static u32 find_visible(ps_store *s, u32 n, u64 L)
{
    u32 count = 0;
    for (u32 i = 0; i < n; i++) {
        ps_ckpt_item *it = &s->ckpt_items[i];
        const ps_page_id id = it->e->id;
        ps_latch(s->pt, id);
        ps_vnode *v = ps_chain_visible(it->e, L);
        PS_ASSERT(v != NULL);
        PS_ASSERT(v->kind == PS_V_FREE || v->frame != PS_NO_FRAME);
        it->v = v;
        it->data = v->kind == PS_V_DATA;
        if (it->data) {
            count++;
        }
        ps_unlatch(s->pt, id);
    }
    return count;
}

/* Set checksums in the I/O buffer so the in-memory page bytes stay unchanged. */
static int write_pages(ps_store *s, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        const ps_ckpt_item *it = &s->ckpt_items[i];
        if (!it->data) {
            continue;
        }
        const ps_page_id id = it->e->id;
        ps_latch(s->pt, id);
        memcpy(s->ckpt_buf, ps_node_page(s, it->v), s->page_size);
        ps_unlatch(s->pt, id);
        ps_page *p = (ps_page *) s->ckpt_buf;
        PS_ASSERT(ps_page_id_eq(p->page_id, id) && p->checksum == 0);
        p->checksum = ps_crc32c(0, s->ckpt_buf, s->page_size);
        const int rv = s->be.ckpt_write(s->be.ctx, id, s->ckpt_buf);
        if (rv != 0) {
            return rv;
        }
    }
    return 0;
}

/*
 * Clear only this checkpoint's bit. A newer commit may have dirtied the
 * same entry on the other list.
 */
static void mark_written(ps_store *s, u32 n, u8 cbit)
{
    for (u32 i = 0; i < n; i++) {
        ps_entry *e = s->ckpt_items[i].e;
        ps_vnode *v = s->ckpt_items[i].v;
        const ps_page_id id = e->id;
        ps_latch(s->pt, id);
        v->flags |= PS_VF_ON_DISK;
        const u8 was = atomic_fetch_and_explicit(&e->dirty, (u8) ~cbit, memory_order_relaxed);
        PS_ASSERT((was & cbit) != 0);
        if ((was & (u8) ~cbit) == 0) {
            atomic_fetch_sub_explicit(&s->frames_dirty, 1, memory_order_relaxed);
        }
        ps_drop_lone_head(s, e);
        ps_update_evictable(s, e);
        ps_entry_maybe_release(s->pt, e);
        ps_unlatch(s->pt, id);
    }
}

int ps_checkpoint_run(ps_store *s)
{
    /* Capture the LSN and hand off the dirty list together under reg_mu. */
    pthread_mutex_lock(&s->reg_mu);
    if (atomic_load_explicit(&s->failed, memory_order_acquire)) {
        pthread_mutex_unlock(&s->reg_mu);
        return EIO;
    }
    const u64 L = atomic_load_explicit(&s->committed_lsn, memory_order_relaxed);
    const u32 c = s->next_idx;
    if (L == atomic_load_explicit(&s->durable_lsn, memory_order_relaxed) &&
        s->dirty_head[c] == NULL) {
        pthread_mutex_unlock(&s->reg_mu);
        return 0;
    }
    PS_ASSERT(!s->ckpt_running && s->dirty_head[c ^ 1] == NULL && s->bmcopy_head == NULL);
    const u32 num_groups = atomic_load_explicit(&s->num_groups, memory_order_relaxed);
    s->ckpt_ref.lsn = L;
    atomic_store_explicit(&s->ckpt_ref.dead, false, memory_order_relaxed);
    ps_reg_add(s, &s->ckpt_ref);
    s->ckpt_running = true;
    s->ckpt_lsn = L;
    s->ckpt_idx = c;
    s->next_idx = c ^ 1;
    pthread_mutex_unlock(&s->reg_mu);

    u32 n = 0;
    int rv = collect(s, c, &n);
    if (rv != 0) {
        return ckpt_failed(s, L, rv, false, false);
    }
    const u32 count = find_visible(s, n, L);

    /* The log must be durable through L before the page image is written. */
    if (s->hooks.flush_log != NULL) {
        rv = s->hooks.flush_log(s->hooks.ctx, L);
        if (rv != 0) {
            return ckpt_failed(s, L, rv, true, false);
        }
    }

    rv = s->be.ckpt_begin(s->be.ctx, L, num_groups, count);
    if (rv != 0) {
        return ckpt_failed(s, L, rv, false, false);
    }
    rv = write_pages(s, n);
    if (rv == 0) {
        bool fatal = false;
        rv = s->be.ckpt_commit(s->be.ctx, &fatal);
        if (rv != 0) {
            return ckpt_failed(s, L, rv, fatal, true);
        }
    }
    if (rv != 0) {
        return ckpt_failed(s, L, rv, false, true);
    }

    /* The bitmap is durable now, so skipped free markers count as on disk. */
    mark_written(s, n, (u8) (1u << c));

    pthread_mutex_lock(&s->reg_mu);
    atomic_store_explicit(&s->durable_lsn, L, memory_order_release);
    s->dirty_head[c] = NULL;
    free_bitmap_copies(s);
    ps_reg_remove(s, &s->ckpt_ref);
    s->ckpt_running = false;
    ps_retire_process(s);
    pthread_mutex_unlock(&s->reg_mu);
    atomic_fetch_add_explicit(&s->checkpoints_run, 1, memory_order_relaxed);
    ps_log(s, PS_LOG_DEBUG, "checkpoint at LSN %llu: %u pages", (unsigned long long) L, count);

    if (s->hooks.checkpoint_done != NULL) {
        s->hooks.checkpoint_done(s->hooks.ctx, L);
    }
    return 0;
}

/* ---- checkpoint thread and waits ----------------------------------------- */

/* Set each wait condition and signal under mem_mu, where waiters check it. */

int ps_ckpt_sync_init(ps_store *s)
{
    int rv = pthread_mutex_init(&s->mem_mu, NULL);
    if (rv != 0) {
        return rv;
    }
    pthread_condattr_t ca;
    rv = pthread_condattr_init(&ca);
    if (rv == 0) {
#ifndef __APPLE__
        rv = pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
#endif
        if (rv == 0) {
            rv = pthread_cond_init(&s->ckpt_thread_cv, &ca);
        }
        pthread_condattr_destroy(&ca);
    }
    if (rv != 0) {
        pthread_mutex_destroy(&s->mem_mu);
        return rv;
    }
    rv = pthread_cond_init(&s->ckpt_wait_cv, NULL);
    if (rv != 0) {
        pthread_cond_destroy(&s->ckpt_thread_cv);
        pthread_mutex_destroy(&s->mem_mu);
        return rv;
    }
    return 0;
}

void ps_ckpt_sync_destroy(ps_store *s)
{
    pthread_cond_destroy(&s->ckpt_wait_cv);
    pthread_cond_destroy(&s->ckpt_thread_cv);
    pthread_mutex_destroy(&s->mem_mu);
}

/* deadline_ms is monotonic. The caller holds mem_mu. */
static void thread_sleep_until(ps_store *s, u64 deadline_ms)
{
    struct timespec ts;
#ifdef __APPLE__
    /* macOS condvars use realtime. Convert the monotonic time remaining. */
    const u64 now = now_ms();
    const u64 rel = deadline_ms > now ? deadline_ms - now : 0;
    clock_gettime(CLOCK_REALTIME, &ts);
    const u64 ns = (u64) ts.tv_nsec + (rel % 1000u) * 1000000u;
    ts.tv_sec += (time_t) (rel / 1000u + ns / 1000000000u);
    ts.tv_nsec = (long) (ns % 1000000000u);
#else
    ts.tv_sec = (time_t) (deadline_ms / 1000u);
    ts.tv_nsec = (long) (deadline_ms % 1000u) * 1000000L;
#endif
    pthread_cond_timedwait(&s->ckpt_thread_cv, &s->mem_mu, &ts);
}

/* Even an empty operation can advance the LSN without dirtying pages. */
static bool work_pending(const ps_store *s)
{
    return atomic_load_explicit(&s->frames_dirty, memory_order_relaxed) > 0 ||
        atomic_load_explicit(&s->committed_lsn, memory_order_acquire) >
        atomic_load_explicit(&s->durable_lsn, memory_order_acquire);
}

/* True when frames_dirty is above checkpoint_dirty_pct of the cache. */
static bool dirty_over(const ps_store *s)
{
    const u64 dirty = atomic_load_explicit(&s->frames_dirty, memory_order_relaxed);
    return dirty * 100u > (u64) s->cfg.checkpoint_dirty_pct * s->total_frames;
}

/*
 * Failed attempts retry after backoff while work is pending, even with
 * the interval timer disabled.
 */
static void *ckpt_thread_main(void *arg)
{
    ps_store *s = arg;
    pthread_mutex_lock(&s->mem_mu);
    while (!s->ckpt_stop) {
        if (atomic_load_explicit(&s->failed, memory_order_acquire)) {
            pthread_cond_wait(&s->ckpt_thread_cv, &s->mem_mu);
            continue;
        }
        const u64 now = now_ms();
        if (s->ckpt_retry_at_ms > now) {
            thread_sleep_until(s, s->ckpt_retry_at_ms);
            continue;
        }
        const u32 iv = s->cfg.checkpoint_interval_ms;
        const u64 due_at = s->ckpt_last_end_ms + iv;
        const bool retry = s->ckpt_last_err != 0 && work_pending(s);
        if (!retry && !s->ckpt_want && !(iv != 0 && now >= due_at) && !dirty_over(s)) {
            if (iv != 0) {
                thread_sleep_until(s, due_at);
            } else {
                pthread_cond_wait(&s->ckpt_thread_cv, &s->mem_mu);
            }
            continue;
        }
        s->ckpt_want = false;
        pthread_mutex_unlock(&s->mem_mu);
        ckpt_attempt(s, true);
        pthread_mutex_lock(&s->mem_mu);
    }
    pthread_mutex_unlock(&s->mem_mu);
    return NULL;
}

int ps_ckpt_thread_start(ps_store *s)
{
    PS_ASSERT(!s->ckpt_thread_started);
    pthread_mutex_lock(&s->mem_mu);
    s->ckpt_stop = false;
    s->ckpt_last_end_ms = now_ms();
    pthread_mutex_unlock(&s->mem_mu);
    const int rv = pthread_create(&s->ckpt_thread, NULL, ckpt_thread_main, s);
    if (rv != 0) {
        return rv;
    }
    s->ckpt_thread_started = true;
    return 0;
}

void ps_ckpt_thread_stop(ps_store *s)
{
    if (!s->ckpt_thread_started) {
        return;
    }
    pthread_mutex_lock(&s->mem_mu);
    s->ckpt_stop = true;
    pthread_cond_signal(&s->ckpt_thread_cv);
    pthread_cond_broadcast(&s->ckpt_wait_cv);
    pthread_mutex_unlock(&s->mem_mu);
    pthread_join(s->ckpt_thread, NULL);
    s->ckpt_thread_started = false;
}

void ps_checkpoint_request(ps_store *s)
{
    if (!s->ckpt_thread_started) {
        return;
    }
    pthread_mutex_lock(&s->mem_mu);
    s->ckpt_want = true;
    pthread_cond_signal(&s->ckpt_thread_cv);
    pthread_mutex_unlock(&s->mem_mu);
}

u32 ps_test_ckpt_waiters(ps_store *s)
{
    pthread_mutex_lock(&s->mem_mu);
    const u32 n = s->ckpt_waiters;
    pthread_mutex_unlock(&s->mem_mu);
    return n;
}

bool ps_test_ckpt_stopping(ps_store *s)
{
    pthread_mutex_lock(&s->mem_mu);
    const bool stop = s->ckpt_stop;
    pthread_mutex_unlock(&s->mem_mu);
    return stop;
}

void ps_ckpt_note_commit(ps_store *s)
{
    if (!s->ckpt_thread_started || !dirty_over(s)) {
        return;
    }
    pthread_mutex_lock(&s->mem_mu);
    pthread_cond_signal(&s->ckpt_thread_cv);
    pthread_mutex_unlock(&s->mem_mu);
}

int ps_ckpt_wait(ps_store *s)
{
    atomic_fetch_add_explicit(&s->writer_waits, 1, memory_order_relaxed);
    pthread_mutex_lock(&s->mem_mu);
    int rv = 0;
    if (atomic_load_explicit(&s->failed, memory_order_acquire) || s->ckpt_stop) {
        rv = EIO;
    } else if (!s->ckpt_inflight && s->ckpt_last_err != 0) {
        /* Give the caller the last error and let the thread retry after backoff. */
        rv = s->ckpt_last_err;
        s->ckpt_want = true;
        pthread_cond_signal(&s->ckpt_thread_cv);
    } else {
        /* Join an attempt already in flight, or request a new one. */
        u64 target = s->ckpt_started;
        if (!s->ckpt_inflight) {
            target++;
            s->ckpt_want = true;
            pthread_cond_signal(&s->ckpt_thread_cv);
        }
        s->ckpt_waiters++;
        while (s->ckpt_done < target && !s->ckpt_stop &&
               !atomic_load_explicit(&s->failed, memory_order_acquire)) {
            pthread_cond_wait(&s->ckpt_wait_cv, &s->mem_mu);
        }
        s->ckpt_waiters--;
        if (atomic_load_explicit(&s->failed, memory_order_acquire) || s->ckpt_done < target) {
            rv = EIO;
        } else {
            rv = s->ckpt_last_err;
        }
    }
    pthread_mutex_unlock(&s->mem_mu);
    return rv;
}
