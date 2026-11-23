#include "mem_backend.h"
#include "pagestore/pagestore.h"
#include "store.h"
#include "test.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PS 4096u
#define HDR PS_PAGE_HEADER_SIZE

static mem_backend *mb;
static ps_store *s;
static u64 next_lsn;

/* ---- log capture ---- */

struct ps_log_ctx {
    pthread_mutex_t mu;
    u32 errors; /* ERROR lines */
    u32 ckpt_errors; /* ERROR lines containing "checkpoint" */
    char last_ckpt_error[512];
};

static void capture_log(ps_log_ctx *lc, int level, const char *msg)
{
    pthread_mutex_lock(&lc->mu);
    if (level == PS_LOG_ERROR) {
        lc->errors++;
        if (strstr(msg, "checkpoint") != NULL) {
            lc->ckpt_errors++;
            snprintf(lc->last_ckpt_error, sizeof(lc->last_ckpt_error), "%s", msg);
        }
    }
    pthread_mutex_unlock(&lc->mu);
}

static ps_log_ctx logs;

static u32 ckpt_errors(void)
{
    pthread_mutex_lock(&logs.mu);
    const u32 n = logs.ckpt_errors;
    pthread_mutex_unlock(&logs.mu);
    return n;
}

/* ---- store setup ---- */

typedef struct thread_cfg {
    u32 frames;
    u32 dirty_pct;
    u32 interval_ms;
} thread_cfg;

static void open_store(thread_cfg tc)
{
    mb = mb_create();
    ps_config cfg = mb_config(tc.frames);
    cfg.checkpoint_thread = true;
    cfg.checkpoint_dirty_pct = tc.dirty_pct;
    cfg.checkpoint_interval_ms = tc.interval_ms;
    pthread_mutex_lock(&logs.mu);
    logs.errors = 0;
    logs.ckpt_errors = 0;
    logs.last_ckpt_error[0] = '\0';
    pthread_mutex_unlock(&logs.mu);
    cfg.log = capture_log;
    cfg.log_ctx = &logs;
    ps_backend be;
    mb_backend(mb, &be);
    T_OK(ps_open_backend(&be, &cfg, PS_CREATE, &s));
    next_lsn = 1;
}

static void close_store(void)
{
    ps_close(s);
    s = NULL;
    mb_destroy(mb);
    mb = NULL;
}

static struct ps_stats stats(void)
{
    struct ps_stats st;
    ps_stats(s, &st);
    return st;
}

static void sleep_ms(u32 ms)
{
    struct timespec ts = {(time_t) (ms / 1000), (long) (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0) {
        /* interrupted: sleep the rest */
    }
}

/* Returns false if the backend has not passed count c within ms. */
static bool wait_ckpt_count_above(u64 c, u32 ms)
{
    const u64 end = t_now_ms() + ms;
    while (mb_ckpt_count(mb) <= c) {
        if (t_now_ms() >= end) {
            return false;
        }
        sleep_ms(2);
    }
    return true;
}

/* The backend commits before the store publishes the durable LSN. */
static bool wait_durable(u64 lsn, u32 ms)
{
    const u64 end = t_now_ms() + ms;
    while (ps_durable_lsn(s) < lsn) {
        if (t_now_ms() >= end) {
            return false;
        }
        sleep_ms(2);
    }
    return true;
}

/* Poll for n ERROR log lines that mention "checkpoint". Returns false after ms. */
static bool wait_ckpt_errors(u32 n, u32 ms)
{
    const u64 end = t_now_ms() + ms;
    while (ckpt_errors() < n) {
        if (t_now_ms() >= end) {
            return false;
        }
        sleep_ms(2);
    }
    return true;
}

static void stamp(ps_page *p, ps_page_id id, u32 tag)
{
    u8 *b = (u8 *) p;
    memcpy(b + HDR, &id.raw, 4);
    memcpy(b + HDR + 4, &tag, 4);
    memset(b + HDR + 8, (int) ((id.raw * 31u + tag) & 0xFFu), PS - HDR - 8);
}

static bool stamped(const ps_page *p, ps_page_id id, u32 tag)
{
    const u8 *b = (const u8 *) p;
    u32 i = 0;
    u32 t = 0;
    memcpy(&i, b + HDR, 4);
    memcpy(&t, b + HDR + 4, 4);
    if (i != id.raw || t != tag || !ps_page_id_eq(p->page_id, id) || p->checksum != 0) {
        return false;
    }
    const u8 want = (u8) ((id.raw * 31u + tag) & 0xFFu);
    for (u32 k = HDR + 8; k < PS; k++) {
        if (b[k] != want) {
            return false;
        }
    }
    return true;
}

/* Commit one operation that allocates a page stamped with tag. Returns its ID. */
static ps_page_id commit_alloc(u32 tag)
{
    T_OK(ps_op_begin(s, next_lsn++));
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    stamp(p, id, tag);
    ps_op_commit(s);
    return id;
}

/* Abort on allocation failure so later attempts can open their own operation. */
static int try_commit_alloc(u32 tag, ps_page_id *id)
{
    T_OK(ps_op_begin(s, next_lsn));
    ps_page *p = NULL;
    const int rv = ps_alloc(s, id, &p);
    if (rv != 0) {
        ps_op_abort(s);
        return rv;
    }
    stamp(p, *id, tag);
    ps_op_commit(s);
    next_lsn++;
    return 0;
}

/* ---- log hooks ---- */

/* Hold flush_log to keep an attempt in flight while another thread acts. */

struct ps_log_hooks_ctx {
    atomic_uint nflush;
    atomic_int flush_rv;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool hold; /* flush_log blocks while set */
    bool held; /* a flush_log is blocked now */
};

static int note_flush(ps_log_hooks_ctx *h, u64 lsn)
{
    PS_UNUSED(lsn);
    atomic_fetch_add(&h->nflush, 1);
    pthread_mutex_lock(&h->mu);
    while (h->hold) {
        h->held = true;
        pthread_cond_wait(&h->cv, &h->mu);
    }
    h->held = false;
    pthread_mutex_unlock(&h->mu);
    return atomic_load(&h->flush_rv);
}

static void set_note_hooks(ps_log_hooks_ctx *h)
{
    atomic_init(&h->nflush, 0);
    atomic_init(&h->flush_rv, 0);
    pthread_mutex_init(&h->mu, NULL);
    pthread_cond_init(&h->cv, NULL);
    h->hold = false;
    h->held = false;
    const ps_log_hooks hooks = {note_flush, NULL, h};
    ps_set_log_hooks(s, &hooks);
}

/* Close first so the background thread has stopped using these hooks. */
static void destroy_note_hooks(ps_log_hooks_ctx *h)
{
    pthread_cond_destroy(&h->cv);
    pthread_mutex_destroy(&h->mu);
}

static void hold_flushes(ps_log_hooks_ctx *h, bool on)
{
    pthread_mutex_lock(&h->mu);
    h->hold = on;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
}

static bool flush_held(ps_log_hooks_ctx *h)
{
    pthread_mutex_lock(&h->mu);
    const bool held = h->held;
    pthread_mutex_unlock(&h->mu);
    return held;
}

/* Wait until the hook is paused so the next step overlaps the checkpoint. */
static void wait_flush_held(ps_log_hooks_ctx *h)
{
    const u64 end = t_now_ms() + 5000;
    while (!flush_held(h)) {
        T_CHECK(t_now_ms() < end);
        sleep_ms(1);
    }
}

/* ps_checkpoint on another thread. */
typedef struct bg_ckpt {
    pthread_t t;
    int rv;
} bg_ckpt;

static void *bg_ckpt_main(void *arg)
{
    bg_ckpt *b = arg;
    b->rv = ps_checkpoint(s);
    return NULL;
}

static void bg_ckpt_start(bg_ckpt *b)
{
    b->rv = -1;
    T_OK(pthread_create(&b->t, NULL, bg_ckpt_main, b));
}

static int bg_ckpt_join(bg_ckpt *b)
{
    T_OK(pthread_join(b->t, NULL));
    return b->rv;
}

/* Direct attempts let the test advance backoff without waiting for the timer. */
static void fail_checkpoints(u32 n, int err)
{
    for (u32 i = 0; i < n; i++) {
        T_ERR(ps_checkpoint(s), err);
    }
}

/* ---- tests ---- */

static void test_dirty_watermark(void)
{
    /* In a 64-frame cache, 26 dirty entries cross the 40% watermark. */
    open_store((thread_cfg) {64, 40, 0});
    const u64 c0 = mb_ckpt_count(mb);
    for (u32 i = 0; i < 20; i++) {
        commit_alloc(1);
    }
    T_EQ(stats().frames_dirty, 21); /* 20 pages and bitmap page 0 */
    sleep_ms(100);
    T_EQ(mb_ckpt_count(mb), c0); /* below the watermark: nothing runs */
    while (stats().frames_dirty <= 25 && mb_ckpt_count(mb) == c0) {
        commit_alloc(1);
    }
    T_CHECK(wait_ckpt_count_above(c0, 2000));
    close_store();
}

static void test_request(void)
{
    open_store((thread_cfg) {64, 100, 0});
    commit_alloc(1);
    const u64 c0 = mb_ckpt_count(mb);
    sleep_ms(50);
    T_EQ(mb_ckpt_count(mb), c0);
    ps_checkpoint_request(s);
    T_CHECK(wait_ckpt_count_above(c0, 1000));
    T_CHECK(wait_durable(1, 1000));
    /* Requests coalesce and a request with nothing to do is harmless. */
    ps_checkpoint_request(s);
    ps_checkpoint_request(s);
    sleep_ms(50);
    T_EQ(mb_ckpt_count(mb), c0 + 1);
    close_store();
}

static void test_request_thread_off(void)
{
    mb = mb_create();
    T_OK(mb_open(mb, 64, PS_CREATE, &s));
    next_lsn = 1;
    commit_alloc(1);
    const u64 c0 = mb_ckpt_count(mb);
    ps_checkpoint_request(s);
    sleep_ms(50);
    T_EQ(mb_ckpt_count(mb), c0);
    close_store();
}

static void test_interval(void)
{
    open_store((thread_cfg) {64, 100, 50});
    const u64 c0 = mb_ckpt_count(mb);
    commit_alloc(1);
    T_CHECK(wait_ckpt_count_above(c0, 1000));
    T_CHECK(wait_durable(1, 1000));
    close_store();
}

static void test_writer_waits(void)
{
    /* Disable automatic triggers so only frame waits start checkpoints. */
    open_store((thread_cfg) {16, 100, 0});
    const u32 n = 60;
    for (u32 i = 0; i < n; i++) {
        T_EQ(commit_alloc(i + 1).raw, i + 1);
    }
    struct ps_stats st = stats();
    T_CHECK(st.writer_waits >= 1);
    T_CHECK(st.checkpoints_run >= 2);
    T_CHECK(!st.failed);

    /* Everything reads back; page-ins of evicted pages wait the same way. */
    for (u32 i = 0; i < n; i++) {
        T_OK(ps_op_begin(s, next_lsn++));
        const ps_page *p = NULL;
        T_OK(ps_read(s, ps_page_id_from(i + 1), &p));
        T_CHECK(stamped(p, ps_page_id_from(i + 1), i + 1));
        ps_op_commit(s);
    }
    close_store();
}

/* Put all n allocations in one operation to share its bitmap spare. */
static void commit_allocs(u32 n, u32 tag)
{
    T_OK(ps_op_begin(s, next_lsn++));
    for (u32 i = 0; i < n; i++) {
        ps_page_id id = PS_PAGE_ID_NONE;
        ps_page *p = NULL;
        T_OK(ps_alloc(s, &id, &p));
        stamp(p, id, tag);
    }
    ps_op_commit(s);
}

/*
 * Fill in two operations to leave room for the bitmap spare. With at most
 * one free frame and no clean pages, the next allocation has to wait.
 */
static void fill_dirty(u32 a, u32 b)
{
    const u64 w0 = stats().writer_waits;
    commit_allocs(a, 1);
    commit_allocs(b, 1);
    const struct ps_stats st = stats();
    T_EQ(st.writer_waits, w0); /* the fill itself did not need a checkpoint */
    T_CHECK(st.frames_free <= 1);
    T_EQ(st.frames_evictable, 0);
}

static void test_reader_waits(void)
{
    open_store((thread_cfg) {16, 100, 0});
    commit_allocs(2, 1);
    T_OK(ps_checkpoint(s));
    T_CHECK(ps_test_evict(s, ps_page_id_from(1)));
    T_CHECK(ps_test_evict(s, ps_page_id_from(2)));
    fill_dirty(7, 7);
    ps_snapshot *sn = NULL;
    T_OK(ps_snapshot_open(s, &sn));
    const ps_page *p = NULL;
    T_EQ(stats().frames_free, 1);
    T_OK(ps_snapshot_read(sn, ps_page_id_from(1), &p)); /* takes the last free frame and pins it */
    T_CHECK(stamped(p, ps_page_id_from(1), 1));
    T_EQ(stats().frames_free, 0);
    T_EQ(stats().frames_evictable, 0);
    const u64 w0 = stats().writer_waits;
    /* waits for a checkpoint instead of EAGAIN */
    T_OK(ps_snapshot_read(sn, ps_page_id_from(2), &p));
    T_CHECK(stamped(p, ps_page_id_from(2), 1));
    T_EQ(stats().writer_waits, w0 + 1);
    T_EQ(stats().snapshots_cut_off, 0);
    T_EQ(stats().frames_dirty, 0);
    ps_snapshot_close(sn);
    close_store();
}

static void test_waiter_gets_error_then_recovers(void)
{
    open_store((thread_cfg) {16, 100, 0});
    fill_dirty(7, 7);
    mb_fail(mb, MB_WRITE, ENOSPC, -1);

    ps_page_id id = PS_PAGE_ID_NONE;
    T_ERR(try_commit_alloc(9, &id), ENOSPC); /* waited, got the attempt's error */
    struct ps_stats st = stats();
    T_EQ(st.checkpoints_failed, 1);
    T_EQ(st.writer_waits, 1);
    T_CHECK(!st.failed);

    /* ENOSPC again, from failing fast or from the thread's retry after its 100 ms backoff. */
    T_ERR(try_commit_alloc(9, &id), ENOSPC);
    st = stats();
    T_CHECK(st.checkpoints_failed >= 1);
    T_EQ(st.writer_waits, 2);

    /* The backoff retries by itself once the cause clears. */
    mb_fail(mb, MB_WRITE, 0, 0);
    const u64 end = t_now_ms() + 5000;
    int rv = ENOSPC;
    while (t_now_ms() < end) {
        rv = try_commit_alloc(9, &id);
        if (rv == 0) {
            break;
        }
        T_ERR(rv, ENOSPC);
        sleep_ms(10);
    }
    T_OK(rv);
    st = stats();
    T_CHECK(!st.failed);
    T_CHECK(st.checkpoints_run >= 2);
    T_CHECK(ps_durable_lsn(s) >= 2);
    close_store();
}

static void test_fail_fast(void)
{
    open_store((thread_cfg) {16, 100, 0});
    fill_dirty(7, 7);
    mb_fail(mb, MB_WRITE, ENOSPC, -1);
    /* Five failures leave a 1.6 s window before the background retry. */
    fail_checkpoints(5, ENOSPC);
    const u64 f0 = stats().checkpoints_failed;
    const u64 t = t_now_ms();
    ps_page_id id = PS_PAGE_ID_NONE;
    T_ERR(try_commit_alloc(9, &id), ENOSPC);
    T_CHECK(t_now_ms() - t < 1000);
    T_EQ(stats().checkpoints_failed, f0); /* no attempt was made for it */
    T_EQ(stats().writer_waits, 1);
    mb_fail(mb, MB_WRITE, 0, 0);
    close_store();
}

/* Releases the held flush_log once a frame request waits in ps_ckpt_wait. */
static void *release_when_waiting(void *arg)
{
    ps_log_hooks_ctx *h = arg;
    const u64 end = t_now_ms() + 5000;
    while (ps_test_ckpt_waiters(s) == 0) {
        T_CHECK(t_now_ms() < end);
        sleep_ms(1);
    }
    hold_flushes(h, false);
    return NULL;
}

static void test_waiter_gets_inflight_result(void)
{
    /* Make the writer wait on an attempt that started before it arrived. */
    open_store((thread_cfg) {16, 100, 0});
    ps_log_hooks_ctx h;
    set_note_hooks(&h);
    fill_dirty(7, 7);
    mb_fail(mb, MB_COMMIT, ENOSPC, -1);
    fail_checkpoints(5, ENOSPC); /* backoff 1.6 s, 3.2 s after the next failure */

    hold_flushes(&h, true);
    bg_ckpt b;
    bg_ckpt_start(&b);
    wait_flush_held(&h); /* in flight, held in flush_log */
    const u64 f0 = stats().checkpoints_failed;
    pthread_t rel;
    T_OK(pthread_create(&rel, NULL, release_when_waiting, &h));

    const u64 t = t_now_ms();
    ps_page_id id = PS_PAGE_ID_NONE;
    T_ERR(try_commit_alloc(9, &id), ENOSPC); /* waits for the held attempt, gets its error */
    T_CHECK(t_now_ms() - t < 1000);
    T_EQ(stats().checkpoints_failed, f0 + 1); /* that attempt only */
    T_EQ(stats().writer_waits, 1);
    T_OK(pthread_join(rel, NULL));
    T_ERR(bg_ckpt_join(&b), ENOSPC);

    /* A success of the attempt in flight: the waiter retries and gets a frame. */
    mb_fail(mb, MB_COMMIT, 0, 0);
    hold_flushes(&h, true);
    bg_ckpt_start(&b);
    wait_flush_held(&h);
    T_OK(pthread_create(&rel, NULL, release_when_waiting, &h));
    T_OK(try_commit_alloc(9, &id));
    T_OK(pthread_join(rel, NULL));
    T_OK(bg_ckpt_join(&b));
    close_store();
    destroy_note_hooks(&h);
}

static void test_fatal_wakes_waiters(void)
{
    open_store((thread_cfg) {16, 100, 0});
    commit_allocs(2, 7);
    T_OK(ps_checkpoint(s));
    T_CHECK(ps_test_evict(s, ps_page_id_from(2)));
    ps_snapshot *sn = NULL;
    T_OK(ps_snapshot_open(s, &sn));
    const ps_page *p1 = NULL;
    T_OK(ps_snapshot_read(sn, ps_page_id_from(1), &p1)); /* resident and pinned: not evictable */
    fill_dirty(7, 6);

    /* The writer is waiting while the slow commit fails fatally. */
    mb_set_commit_delay_ms(mb, 200);
    mb_fail(mb, MB_COMMIT_FATAL, EIO, 1);
    ps_page_id id = PS_PAGE_ID_NONE;
    T_ERR(try_commit_alloc(9, &id), EIO);
    struct ps_stats st = stats();
    T_CHECK(st.failed);
    T_EQ(st.writer_waits, 1);
    /* Waiters are woken before the checkpoint logs its failure. */
    T_CHECK(wait_ckpt_errors(1, 2000));

    /* The existing snapshot still reads its resident page. */
    const ps_page *p = NULL;
    T_OK(ps_snapshot_read(sn, ps_page_id_from(1), &p));
    T_CHECK(p == p1);
    T_CHECK(stamped(p, ps_page_id_from(1), 7));

    /* Everything else is EIO, at once. */
    const u64 t = t_now_ms();
    T_ERR(ps_op_begin(s, next_lsn), EIO);
    T_ERR(ps_checkpoint(s), EIO);
    ps_snapshot *sn2 = NULL;
    T_ERR(ps_snapshot_open(s, &sn2), EIO);
    /* evicted: a failed store reads no page */
    T_ERR(ps_snapshot_read(sn, ps_page_id_from(2), &p), EIO);
    ps_checkpoint_request(s); /* a no-op */
    T_CHECK(t_now_ms() - t < 1000);
    ps_snapshot_close(sn);
    close_store();
}

static void test_flush_log_failure_wakes_waiters(void)
{
    /* A failed log flush in the checkpoint thread fails the store. The waiter gets EIO. */
    open_store((thread_cfg) {16, 100, 0});
    ps_log_hooks_ctx h;
    set_note_hooks(&h);
    fill_dirty(7, 7);
    atomic_store(&h.flush_rv, ENOSPC);
    ps_page_id id = PS_PAGE_ID_NONE;
    T_ERR(try_commit_alloc(9, &id), EIO);
    T_CHECK(stats().failed);
    T_EQ(atomic_load(&h.nflush), 1);
    close_store();
    destroy_note_hooks(&h);
}

static void test_log_on_failure(void)
{
    open_store((thread_cfg) {64, 100, 0});
    commit_alloc(1);
    mb_fail(mb, MB_WRITE, ENOSPC, 1);
    ps_checkpoint_request(s);
    T_CHECK(wait_ckpt_errors(1, 2000));
    T_EQ(ckpt_errors(), 1);
    pthread_mutex_lock(&logs.mu);
    T_CHECK(strstr(logs.last_ckpt_error, strerror(ENOSPC)) != NULL);
    pthread_mutex_unlock(&logs.mu);
    struct ps_stats st = stats();
    T_EQ(st.checkpoints_failed, 1);
    T_CHECK(!st.failed);

    /* With no trigger (no timer, no watermark, no request) the thread retries by itself. */
    T_CHECK(wait_durable(1, 2000));
    T_EQ(ckpt_errors(), 1);
    close_store();
}

/* Releases the held flush_log once ps_close has asked the thread to stop. */
typedef struct close_release {
    ps_log_hooks_ctx *h;
    u64 released_ms;
} close_release;

static void *release_when_closing(void *arg)
{
    close_release *cr = arg;
    const u64 end = t_now_ms() + 5000;
    while (!ps_test_ckpt_stopping(s)) {
        T_CHECK(t_now_ms() < end);
        sleep_ms(1);
    }
    sleep_ms(50);
    T_CHECK(flush_held(cr->h)); /* still in flight while close waits */
    cr->released_ms = t_now_ms();
    hold_flushes(cr->h, false);
    return NULL;
}

static void test_close_during_checkpoint(void)
{
    open_store((thread_cfg) {64, 100, 0});
    ps_log_hooks_ctx h;
    set_note_hooks(&h);
    commit_alloc(1);
    commit_alloc(2);
    const u64 c0 = mb_ckpt_count(mb);
    hold_flushes(&h, true);
    ps_checkpoint_request(s);
    wait_flush_held(&h); /* the thread's checkpoint is in flight */
    close_release cr = {&h, 0};
    pthread_t rel;
    T_OK(pthread_create(&rel, NULL, release_when_closing, &cr));
    const u64 t = t_now_ms();
    ps_close(s);
    const u64 done = t_now_ms();
    T_OK(pthread_join(rel, NULL));
    s = NULL;
    T_CHECK(cr.released_ms != 0 && done >= cr.released_ms); /* close waited for the attempt */
    T_CHECK(done - t < 2000);
    T_EQ(mb_ckpt_count(mb), c0 + 1); /* it finished before the store was torn down */
    T_EQ(mb_info(mb).durable_lsn, 2);
    mb_destroy(mb);
    mb = NULL;
    destroy_note_hooks(&h);
}

static void test_close_during_backoff(void)
{
    open_store((thread_cfg) {64, 100, 0});
    commit_alloc(1);
    mb_fail(mb, MB_WRITE, ENOSPC, -1);
    fail_checkpoints(7, ENOSPC); /* backoff 6.4 s */
    const u64 t = t_now_ms();
    close_store();
    T_CHECK(t_now_ms() - t < 1000);
}

static void test_close_idle_thread(void)
{
    open_store((thread_cfg) {64, 40, 60000});
    commit_alloc(1);
    const u64 t = t_now_ms();
    close_store();
    T_CHECK(t_now_ms() - t < 1000);
}

static void test_close_failed_no_leak(void)
{
    open_store((thread_cfg) {64, 100, 0});
    commit_alloc(1);
    commit_alloc(2);
    T_OK(ps_checkpoint(s));
    /* Leave an old page version and a bitmap copy for teardown to free. */
    T_OK(ps_op_begin(s, next_lsn++));
    ps_page *p = NULL;
    T_OK(ps_write(s, ps_page_id_from(1), &p));
    stamp(p, ps_page_id_from(1), 3);
    ps_page_id id = PS_PAGE_ID_NONE;
    T_OK(ps_alloc(s, &id, &p));
    stamp(p, id, 3);
    T_OK(ps_free(s, ps_page_id_from(2)));
    ps_op_commit(s);

    mb_fail(mb, MB_COMMIT_FATAL, EIO, 1);
    ps_checkpoint_request(s);
    const u64 end = t_now_ms() + 2000;
    while (!stats().failed && t_now_ms() < end) {
        sleep_ms(2);
    }
    T_CHECK(stats().failed);
    const u64 t = t_now_ms();
    close_store();
    T_CHECK(t_now_ms() - t < 1000);
}

static void test_open_error_no_thread(void)
{
    /* A failed open (format's checkpoint fails) starts no thread and leaks nothing. */
    mb = mb_create();
    mb_fail(mb, MB_WRITE, ENOSPC, 1);
    ps_config cfg = mb_config(64);
    cfg.checkpoint_thread = true;
    ps_backend be;
    mb_backend(mb, &be);
    T_ERR(ps_open_backend(&be, &cfg, PS_CREATE, &s), ENOSPC);
    mb_destroy(mb);
    mb = NULL;
    s = NULL;
}

static void test_many_waits(void)
{
    /* Lost-wakeup check: many short waits race requests from another thread. None may hang. */
    open_store((thread_cfg) {16, 100, 0});
    const u32 n = 300 * t_scale();
    for (u32 i = 0; i < n; i++) {
        if (i % 7 == 0) {
            ps_checkpoint_request(s);
        }
        commit_alloc(1);
    }
    T_CHECK(stats().writer_waits >= 1);
    close_store();
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    pthread_mutex_init(&logs.mu, NULL);
    T_RUN(test_dirty_watermark);
    T_RUN(test_request);
    T_RUN(test_request_thread_off);
    T_RUN(test_interval);
    T_RUN(test_writer_waits);
    T_RUN(test_reader_waits);
    T_RUN(test_waiter_gets_error_then_recovers);
    T_RUN(test_fail_fast);
    T_RUN(test_waiter_gets_inflight_result);
    T_RUN(test_fatal_wakes_waiters);
    T_RUN(test_flush_log_failure_wakes_waiters);
    T_RUN(test_log_on_failure);
    T_RUN(test_close_during_checkpoint);
    T_RUN(test_close_during_backoff);
    T_RUN(test_close_idle_thread);
    T_RUN(test_close_failed_no_leak);
    T_RUN(test_open_error_no_thread);
    T_RUN(test_many_waits);
    pthread_mutex_destroy(&logs.mu);
    return t_done();
}
