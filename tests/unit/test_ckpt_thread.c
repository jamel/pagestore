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

static void stamp(ps_page *p, u32 id, u32 tag)
{
    u8 *b = (u8 *) p;
    memcpy(b + HDR, &id, 4);
    memcpy(b + HDR + 4, &tag, 4);
    memset(b + HDR + 8, (int) ((id * 31u + tag) & 0xFFu), PS - HDR - 8);
}

static bool stamped(const ps_page *p, u32 id, u32 tag)
{
    const u8 *b = (const u8 *) p;
    u32 i = 0;
    u32 t = 0;
    memcpy(&i, b + HDR, 4);
    memcpy(&t, b + HDR + 4, 4);
    if (i != id || t != tag || p->page_id != id || p->checksum != 0) {
        return false;
    }
    const u8 want = (u8) ((id * 31u + tag) & 0xFFu);
    for (u32 k = HDR + 8; k < PS; k++) {
        if (b[k] != want) {
            return false;
        }
    }
    return true;
}

/* Commit one operation that allocates a page stamped with tag. Returns its ID. */
static u32 commit_alloc(u32 tag)
{
    T_OK(ps_op_begin(s, next_lsn++));
    u32 id = 0;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    stamp(p, id, tag);
    ps_op_commit(s);
    return id;
}

/* Abort on allocation failure so later attempts can open their own operation. */
static int try_commit_alloc(u32 tag, u32 *id)
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

/* ---- log hooks: flush_log records that a checkpoint started ---------------- */

struct ps_log_hooks_ctx {
    atomic_uint nflush;
    atomic_int flush_rv;
};

static int note_flush(ps_log_hooks_ctx *h, u64 lsn)
{
    PS_UNUSED(lsn);
    atomic_fetch_add(&h->nflush, 1);
    return atomic_load(&h->flush_rv);
}

static void set_note_hooks(ps_log_hooks_ctx *h)
{
    atomic_init(&h->nflush, 0);
    atomic_init(&h->flush_rv, 0);
    const ps_log_hooks hooks = {note_flush, NULL, h};
    ps_set_log_hooks(s, &hooks);
}

static bool wait_flushes(ps_log_hooks_ctx *h, u32 n, u32 ms)
{
    const u64 end = t_now_ms() + ms;
    while (atomic_load(&h->nflush) < n) {
        if (t_now_ms() >= end) {
            return false;
        }
        sleep_ms(1);
    }
    return true;
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
        T_EQ(commit_alloc(i + 1), i + 1);
    }
    struct ps_stats st = stats();
    T_CHECK(st.writer_waits >= 1);
    T_CHECK(st.checkpoints_run >= 2);
    T_CHECK(!st.failed);

    /* Everything reads back; page-ins of evicted pages wait the same way. */
    for (u32 i = 0; i < n; i++) {
        T_OK(ps_op_begin(s, next_lsn++));
        const ps_page *p = NULL;
        T_OK(ps_read(s, i + 1, &p));
        T_CHECK(stamped(p, i + 1, i + 1));
        ps_op_commit(s);
    }
    close_store();
}

/* Put all n allocations in one operation to share its bitmap spare. */
static void commit_allocs(u32 n, u32 tag)
{
    T_OK(ps_op_begin(s, next_lsn++));
    for (u32 i = 0; i < n; i++) {
        u32 id = 0;
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
    T_CHECK(ps_test_evict(s, 1));
    T_CHECK(ps_test_evict(s, 2));
    fill_dirty(7, 7);
    ps_snapshot *sn = NULL;
    T_OK(ps_snapshot_open(s, &sn));
    const ps_page *p = NULL;
    T_EQ(stats().frames_free, 1);
    T_OK(ps_snapshot_read(sn, 1, &p)); /* takes the last free frame and pins it */
    T_CHECK(stamped(p, 1, 1));
    T_EQ(stats().frames_free, 0);
    T_EQ(stats().frames_evictable, 0);
    const u64 w0 = stats().writer_waits;
    T_OK(ps_snapshot_read(sn, 2, &p)); /* waits for a checkpoint instead of EAGAIN */
    T_CHECK(stamped(p, 2, 1));
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

    u32 id = 0;
    T_ERR(try_commit_alloc(9, &id), ENOSPC); /* waited, got the attempt's error */
    struct ps_stats st = stats();
    T_EQ(st.checkpoints_failed, 1);
    T_EQ(st.writer_waits, 1);
    T_CHECK(!st.failed);

    /* No attempt in flight and the last one failed: fail fast, no new attempt waited for. */
    T_ERR(try_commit_alloc(9, &id), ENOSPC);
    st = stats();
    T_EQ(st.checkpoints_failed, 1);
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

static void test_fatal_wakes_waiters(void)
{
    open_store((thread_cfg) {16, 100, 0});
    commit_allocs(2, 7);
    T_OK(ps_checkpoint(s));
    T_CHECK(ps_test_evict(s, 2));
    ps_snapshot *sn = NULL;
    T_OK(ps_snapshot_open(s, &sn));
    const ps_page *p1 = NULL;
    T_OK(ps_snapshot_read(sn, 1, &p1)); /* resident and pinned: not evictable */
    fill_dirty(7, 6);

    /* The writer is waiting while the slow commit fails fatally. */
    mb_set_commit_delay_ms(mb, 200);
    mb_fail(mb, MB_COMMIT_FATAL, EIO, 1);
    u32 id = 0;
    T_ERR(try_commit_alloc(9, &id), EIO);
    struct ps_stats st = stats();
    T_CHECK(st.failed);
    T_EQ(st.writer_waits, 1);
    /* Waiters are woken before the checkpoint logs its failure. */
    T_CHECK(wait_ckpt_errors(1, 2000));

    /* The existing snapshot still reads its resident page. */
    const ps_page *p = NULL;
    T_OK(ps_snapshot_read(sn, 1, &p));
    T_CHECK(p == p1);
    T_CHECK(stamped(p, 1, 7));

    /* Everything else is EIO, at once. */
    const u64 t = t_now_ms();
    T_ERR(ps_op_begin(s, next_lsn), EIO);
    T_ERR(ps_checkpoint(s), EIO);
    ps_snapshot *sn2 = NULL;
    T_ERR(ps_snapshot_open(s, &sn2), EIO);
    T_ERR(ps_snapshot_read(sn, 2, &p), EIO); /* evicted: a failed store reads no page */
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
    u32 id = 0;
    T_ERR(try_commit_alloc(9, &id), EIO);
    T_CHECK(stats().failed);
    T_EQ(atomic_load(&h.nflush), 1);
    close_store();
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

    /* A later request succeeds (after the backoff). */
    const u64 c0 = mb_ckpt_count(mb);
    ps_checkpoint_request(s);
    T_CHECK(wait_ckpt_count_above(c0, 2000));
    T_CHECK(wait_durable(1, 1000));
    close_store();
}

static void test_close_during_checkpoint(void)
{
    open_store((thread_cfg) {64, 100, 0});
    ps_log_hooks_ctx h;
    set_note_hooks(&h);
    commit_alloc(1);
    commit_alloc(2);
    const u64 c0 = mb_ckpt_count(mb);
    mb_set_commit_delay_ms(mb, 300);
    ps_checkpoint_request(s);
    T_CHECK(wait_flushes(&h, 1, 2000)); /* the checkpoint is in flight */
    sleep_ms(50);
    const u64 t = t_now_ms();
    ps_close(s);
    s = NULL;
    T_CHECK(t_now_ms() - t < 2000);
    T_EQ(mb_ckpt_count(mb), c0 + 1); /* it finished before the store was torn down */
    T_EQ(mb_info(mb).durable_lsn, 2);
    mb_destroy(mb);
    mb = NULL;
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
    T_OK(ps_write(s, 1, &p));
    stamp(p, 1, 3);
    u32 id = 0;
    T_OK(ps_alloc(s, &id, &p));
    stamp(p, id, 3);
    T_OK(ps_free(s, 2));
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
    T_RUN(test_fatal_wakes_waiters);
    T_RUN(test_flush_log_failure_wakes_waiters);
    T_RUN(test_log_on_failure);
    T_RUN(test_close_during_checkpoint);
    T_RUN(test_close_idle_thread);
    T_RUN(test_close_failed_no_leak);
    T_RUN(test_open_error_no_thread);
    T_RUN(test_many_waits);
    pthread_mutex_destroy(&logs.mu);
    return t_done();
}
