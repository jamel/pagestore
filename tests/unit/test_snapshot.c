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

#define HDR PS_PAGE_HEADER_SIZE

static mem_backend *mb;
static ps_store *s;

static void open_store(u32 frames)
{
    mb = mb_create();
    T_OK(mb_open(mb, frames, PS_CREATE, &s));
}

static void open_cfg(const ps_config *cfg)
{
    mb = mb_create();
    ps_backend be;
    mb_backend(mb, &be);
    T_OK(ps_open_backend(&be, cfg, PS_CREATE, &s));
}

static void reopen(u32 frames)
{
    ps_close(s);
    s = NULL;
    T_OK(mb_open(mb, frames, 0, &s));
}

static void close_store(void)
{
    ps_close(s);
    s = NULL;
    mb_destroy(mb);
    mb = NULL;
}

static void sleep_ms(u32 ms)
{
    struct timespec ts = {(time_t) (ms / 1000), (long) (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0) {}
}

static struct ps_stats stats(void)
{
    struct ps_stats st;
    ps_stats(s, &st);
    return st;
}

static u32 alloc_text(const char *text)
{
    u32 id = 0;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    memcpy((u8 *) p + HDR, text, strlen(text) + 1);
    return id;
}

static u32 alloc_with(u64 lsn, const char *text)
{
    T_OK(ps_op_begin(s, lsn));
    const u32 id = alloc_text(text);
    ps_op_commit(s);
    return id;
}

static void write_text(u32 id, const char *text)
{
    ps_page *p = NULL;
    T_OK(ps_write(s, id, &p));
    memcpy((u8 *) p + HDR, text, strlen(text) + 1);
}

static void write_with(u64 lsn, u32 id, const char *text)
{
    T_OK(ps_op_begin(s, lsn));
    write_text(id, text);
    ps_op_commit(s);
}

static void free_with(u64 lsn, u32 id)
{
    T_OK(ps_op_begin(s, lsn));
    T_OK(ps_free(s, id));
    ps_op_commit(s);
}

static ps_snapshot *snap_open(void)
{
    ps_snapshot *sn = NULL;
    T_OK(ps_snapshot_open(s, &sn));
    T_CHECK(sn != NULL);
    return sn;
}

static const ps_page *snap_get(ps_snapshot *sn, u32 id)
{
    const ps_page *p = NULL;
    T_OK(ps_snapshot_read(sn, id, &p));
    T_CHECK(p != NULL);
    T_EQ(p->page_id, id);
    T_EQ(p->checksum, 0);
    T_CHECK(p->lsn <= ps_snapshot_lsn(sn));
    return p;
}

static bool has_text(const ps_page *p, const char *text)
{
    return strcmp((const char *) p + HDR, text) == 0;
}

static void snap_check(ps_snapshot *sn, u32 id, const char *text)
{
    T_CHECK(has_text(snap_get(sn, id), text));
}

static void op_check(u64 lsn, u32 id, const char *text)
{
    T_OK(ps_op_begin(s, lsn));
    const ps_page *p = NULL;
    T_OK(ps_read(s, id, &p));
    T_CHECK(has_text(p, text));
    ps_op_commit(s);
}

/* Reopen so the user pages at LSN 1 have to be read from disk. */
static void disk_pages(u32 frames, u32 n)
{
    open_store(frames);
    T_OK(ps_op_begin(s, 1));
    for (u32 i = 1; i <= n; i++) {
        char text[16];
        snprintf(text, sizeof(text), "P%u", i);
        T_EQ(alloc_text(text), i);
    }
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    reopen(frames);
    for (u32 i = 1; i <= n; i++) {
        T_CHECK(!ps_test_has_entry(s, i));
    }
}

/* ---- checkpoint start signal ---- */

struct ps_log_hooks_ctx {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    u32 nflush;
};

static int rec_flush(ps_log_hooks_ctx *h, u64 lsn)
{
    PS_UNUSED(lsn);
    pthread_mutex_lock(&h->mu);
    h->nflush++;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
    return 0;
}

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

/* ---- tests ---- */

static void test_snapshot_sees_old(void)
{
    open_store(64);
    T_EQ(alloc_with(1, "A"), 1);
    ps_snapshot *s1 = snap_open();
    write_with(2, 1, "B");
    T_EQ(stats().frames_retired, 1);
    snap_check(s1, 1, "A");
    ps_snapshot *s2 = snap_open();
    snap_check(s2, 1, "B");
    T_EQ(stats().snapshots_open, 2);
    ps_snapshot_close(s1);
    T_EQ(stats().frames_retired, 0);
    T_EQ(ps_test_chain_len(s, 1), 1);
    snap_check(s2, 1, "B");
    ps_snapshot_close(s2);
    T_EQ(stats().snapshots_open, 0);
    T_EQ(ps_test_pins(s, 1), 0);
    close_store();
}

static void test_open_mid_operation(void)
{
    open_store(64);
    T_EQ(alloc_with(1, "A"), 1);
    T_OK(ps_op_begin(s, 2));
    write_text(1, "B");
    ps_snapshot *sn = snap_open();
    T_EQ(ps_snapshot_lsn(sn), 1);
    snap_check(sn, 1, "A");
    ps_op_commit(s);
    snap_check(sn, 1, "A");
    ps_snapshot_unpin(sn, 1);
    snap_check(sn, 1, "A"); /* walks the chain again */
    ps_snapshot_close(sn);
    T_EQ(stats().frames_retired, 0);
    close_store();
}

static void test_free_keeps_old(void)
{
    open_store(64);
    T_EQ(alloc_with(1, "A"), 1);
    ps_snapshot *sn = snap_open();
    free_with(2, 1);
    snap_check(sn, 1, "A");
    ps_snapshot *s2 = snap_open();
    const ps_page *p = NULL;
    T_ERR(ps_snapshot_read(s2, 1, &p), EINVAL);
    ps_snapshot_close(s2);
    ps_snapshot_close(sn);
    T_EQ(stats().frames_retired, 0);
    T_EQ(ps_test_chain_len(s, 1), 1); /* the marker, until a checkpoint */
    T_OK(ps_checkpoint(s));
    T_CHECK(!ps_test_has_entry(s, 1));
    close_store();
}

static void test_pin_survives_cut_off(void)
{
    open_store(64);
    T_EQ(alloc_with(1, "A"), 1);
    T_EQ(alloc_with(2, "X"), 2);
    T_CHECK(!ps_snap_cut_off_oldest(s));
    const u32 free0 = stats().frames_free;

    ps_snapshot *sn = snap_open();
    const ps_page *p = snap_get(sn, 1);
    T_CHECK(has_text(p, "A"));
    T_CHECK(ps_snap_cut_off_oldest(s));
    T_EQ(stats().snapshots_cut_off, 1);
    T_EQ(stats().snapshots_open, 1);
    T_CHECK(!ps_snap_cut_off_oldest(s));

    write_with(3, 1, "B");
    T_EQ(stats().frames_retired, 0); /* nobody registered needs A */
    T_EQ(ps_test_chain_len(s, 1), 2); /* A stays for the pin */
    T_CHECK(has_text(p, "A"));
    const ps_page *q = NULL;
    T_ERR(ps_snapshot_read(sn, 2, &q), ESTALE);
    T_ERR(ps_snapshot_read(sn, 1, &q), ESTALE);
    T_CHECK(has_text(p, "A"));

    ps_snapshot_close(sn);
    T_EQ(stats().frames_retired, 0);
    T_EQ(stats().frames_free, free0);
    T_EQ(stats().snapshots_open, 0);
    T_EQ(ps_test_chain_len(s, 1), 1);
    op_check(4, 1, "B");
    close_store();
}

static void test_same_id_one_pin(void)
{
    ps_config cfg = mb_config(64);
    cfg.snapshot_pin_cap = 1;
    open_cfg(&cfg);
    T_EQ(alloc_with(1, "A"), 1);
    T_EQ(alloc_with(2, "B"), 2);

    ps_snapshot *sn = snap_open();
    const ps_page *a = snap_get(sn, 1);
    const ps_page *b = snap_get(sn, 1);
    T_CHECK(a == b);
    T_EQ(ps_test_pins(s, 1), 1);
    T_EQ(stats().frames_pinned, 1);
    const ps_page *p = NULL;
    T_ERR(ps_snapshot_read(sn, 2, &p), ENOBUFS);
    ps_snapshot_unpin(sn, 1);
    T_EQ(ps_test_pins(s, 1), 0);
    snap_check(sn, 2, "B");
    ps_snapshot_close(sn);
    T_EQ(ps_test_pins(s, 2), 0);
    T_EQ(stats().frames_pinned, 0);
    close_store();
}

static void test_pin_cap(void)
{
    ps_config cfg = mb_config(64);
    cfg.snapshot_pin_cap = 4;
    open_cfg(&cfg);
    T_OK(ps_op_begin(s, 1));
    for (u32 i = 1; i <= 5; i++) {
        T_EQ(alloc_text("x"), i);
    }
    ps_op_commit(s);

    ps_snapshot *sn = snap_open();
    for (u32 i = 1; i <= 4; i++) {
        snap_get(sn, i);
    }
    const ps_page *p = NULL;
    T_ERR(ps_snapshot_read(sn, 5, &p), ENOBUFS);
    ps_snapshot_unpin(sn, 2);
    T_EQ(ps_test_pins(s, 2), 0);
    snap_get(sn, 5);
    T_ERR(ps_snapshot_read(sn, 2, &p), ENOBUFS);
    ps_snapshot_close(sn);
    for (u32 i = 1; i <= 5; i++) {
        T_EQ(ps_test_pins(s, i), 0);
    }
    close_store();
}

static void test_global_pin_limit(void)
{
    open_store(32); /* max_pinned = 8 */
    T_OK(ps_op_begin(s, 1));
    for (u32 i = 1; i <= 9; i++) {
        T_EQ(alloc_text("x"), i);
    }
    ps_op_commit(s);

    ps_snapshot *s1 = snap_open();
    ps_snapshot *s2 = snap_open();
    for (u32 i = 1; i <= 5; i++) {
        snap_get(s1, i);
    }
    for (u32 i = 6; i <= 8; i++) {
        snap_get(s2, i);
    }
    const ps_page *p = NULL;
    T_ERR(ps_snapshot_read(s2, 9, &p), ENOBUFS);
    T_ERR(ps_snapshot_read(s1, 9, &p), ENOBUFS);
    snap_get(s2, 6); /* already pinned: no new pin needed */
    ps_snapshot_unpin(s1, 1);
    snap_get(s2, 9);
    T_ERR(ps_snapshot_read(s1, 1, &p), ENOBUFS);
    ps_snapshot_close(s2);
    snap_get(s1, 1);
    ps_snapshot_close(s1);
    close_store();
}

static void test_snapshot_bitmap_einval(void)
{
    open_store(64);
    T_EQ(alloc_with(1, "A"), 1);
    ps_snapshot *sn = snap_open();
    const ps_page *p = NULL;
    T_ERR(ps_snapshot_read(sn, 0, &p), EINVAL);
    T_ERR(ps_snapshot_read(sn, 32576, &p), EINVAL); /* group 1's bitmap id */
    T_ERR(ps_snapshot_read(sn, UINT32_MAX, &p), EINVAL);
    snap_check(sn, 1, "A");
    ps_snapshot_close(sn);
    close_store();
}

/* A resident chain can show that a page was allocated after the snapshot LSN. */
static void test_not_yet_allocated_einval(void)
{
    open_store(64);
    T_EQ(alloc_with(1, "A"), 1);
    ps_snapshot *sn = snap_open();
    T_EQ(alloc_with(2, "B"), 2);
    const ps_page *p = NULL;
    T_ERR(ps_snapshot_read(sn, 2, &p), EINVAL);
    T_OK(ps_op_begin(s, 3));
    T_EQ(alloc_text("C"), 3);
    T_ERR(ps_snapshot_read(sn, 3, &p), EINVAL); /* only an in-op version */
    ps_op_abort(s);
    ps_snapshot_close(sn);
    close_store();
}

static void test_checkpoint_respects_snapshot(void)
{
    open_store(64);
    T_EQ(alloc_with(1, "A"), 1);
    ps_snapshot *sn = snap_open();
    write_with(2, 1, "B");
    write_with(3, 1, "C");
    T_EQ(ps_test_chain_len(s, 1), 2); /* B was needed by nobody */
    T_OK(ps_checkpoint(s));
    T_EQ(stats().frames_retired, 1);
    snap_check(sn, 1, "A");
    ps_snapshot_close(sn);
    T_EQ(stats().frames_retired, 0);
    T_EQ(ps_test_chain_len(s, 1), 1);
    close_store();
}

static void test_alloc_loads_frameless_old(void)
{
    disk_pages(64, 5);
    ps_snapshot *sn = snap_open();
    free_with(2, 5);
    T_EQ(ps_test_chain_len(s, 5), 2); /* marker over the lsn-0 disk node, kept for sn */
    T_EQ(stats().frames_retired, 0); /* frameless */
    const u64 misses = stats().cache_misses;

    T_OK(ps_op_begin(s, 3));
    T_EQ(alloc_text("C"), 5);
    T_EQ(stats().cache_misses, misses + 1); /* the old version was loaded */
    ps_op_commit(s);
    T_EQ(stats().frames_retired, 1);
    T_OK(ps_checkpoint(s));
    T_CHECK(has_text((const ps_page *) mb_page(mb, 5), "C"));

    snap_check(sn, 5, "P5");
    T_EQ(stats().cache_misses, misses + 1);
    ps_snapshot_close(sn);
    T_EQ(stats().frames_retired, 0);
    T_EQ(ps_test_chain_len(s, 5), 1);
    op_check(4, 5, "C");
    close_store();
}

/* Count the retired frame only once page-in supplies one. */
static void test_pagein_retired_counts(void)
{
    disk_pages(64, 5);
    const u32 free0 = stats().frames_free;
    ps_snapshot *sn = snap_open();
    free_with(2, 5);
    T_EQ(stats().frames_retired, 0);
    snap_check(sn, 5, "P5");
    T_EQ(stats().frames_retired, 1);
    T_EQ(stats().frames_free, free0 - 1);
    ps_snapshot_close(sn);
    T_EQ(stats().frames_retired, 0);
    T_EQ(stats().frames_free, free0);
    T_EQ(ps_test_chain_len(s, 5), 1); /* the marker */
    close_store();
}

static void test_retired_freed_at_checkpoint_end(void)
{
    open_store(64);
    ps_log_hooks_ctx h;
    memset(&h, 0, sizeof(h));
    pthread_mutex_init(&h.mu, NULL);
    pthread_cond_init(&h.cv, NULL);
    const ps_log_hooks hooks = {rec_flush, NULL, &h};
    ps_set_log_hooks(s, &hooks);
    T_EQ(alloc_with(1, "A"), 1);
    const u32 free0 = stats().frames_free;

    mb_block_writes(mb, true);
    bg_ckpt b = {0};
    T_OK(pthread_create(&b.t, NULL, bg_ckpt_main, &b));
    pthread_mutex_lock(&h.mu);
    while (h.nflush == 0) {
        pthread_cond_wait(&h.cv, &h.mu);
    }
    pthread_mutex_unlock(&h.mu);

    write_with(2, 1, "B"); /* A is kept for the checkpoint at 1 */
    T_EQ(stats().frames_retired, 1);
    write_with(3, 1, "C"); /* B is not */
    T_EQ(stats().frames_retired, 1);
    T_EQ(ps_test_chain_len(s, 1), 2);
    mb_block_writes(mb, false);
    T_OK(pthread_join(b.t, NULL));
    T_OK(b.rv);
    T_EQ(stats().frames_retired, 0);
    T_EQ(stats().frames_free, free0);
    T_EQ(ps_test_chain_len(s, 1), 1);
    T_CHECK(has_text((const ps_page *) mb_page(mb, 1), "A"));

    ps_set_log_hooks(s, NULL);
    pthread_cond_destroy(&h.cv);
    pthread_mutex_destroy(&h.mu);
    close_store();
}

static void test_snapshot_lsn(void)
{
    open_store(64);
    alloc_with(4, "A");
    ps_snapshot *sn = snap_open();
    T_EQ(ps_snapshot_lsn(sn), 4);
    T_EQ(ps_snapshot_lsn(sn), ps_committed_lsn(s));
    alloc_with(7, "B");
    T_EQ(ps_snapshot_lsn(sn), 4);
    ps_snapshot *s2 = snap_open();
    T_EQ(ps_snapshot_lsn(s2), 7);
    ps_snapshot_close(s2);
    ps_snapshot_close(sn);
    close_store();
}

/* Check pin accounting for pages loaded from the backend. */
static void test_pagein_and_close(void)
{
    disk_pages(64, 4);
    ps_snapshot *sn = snap_open();
    const u64 misses = stats().cache_misses;
    snap_check(sn, 1, "P1");
    snap_check(sn, 2, "P2");
    snap_check(sn, 3, "P3");
    T_EQ(stats().cache_misses, misses + 3);
    T_EQ(stats().frames_pinned, 3);
    ps_snapshot_unpin(sn, 2);
    T_EQ(stats().frames_pinned, 2);
    snap_check(sn, 2, "P2"); /* resident now: a hit */
    T_EQ(stats().cache_misses, misses + 3);
    ps_snapshot_close(sn);
    T_EQ(stats().frames_pinned, 0);
    for (u32 i = 1; i <= 3; i++) {
        T_EQ(ps_test_pins(s, i), 0);
    }
    T_EQ(stats().frames_evictable, 3);
    close_store();
}

/*
 * Load the disk node under a marker in the open operation. Abort must leave the
 * read pointer valid after that node becomes the head again.
 */
static void test_abort_keeps_pinned_base(void)
{
    disk_pages(64, 5);
    T_OK(ps_op_begin(s, 2));
    T_OK(ps_free(s, 5));
    ps_snapshot *sn = snap_open();
    const ps_page *p = snap_get(sn, 5);
    T_CHECK(has_text(p, "P5"));
    ps_op_abort(s);
    T_EQ(ps_test_chain_len(s, 5), 1);
    T_EQ(ps_test_pins(s, 5), 1);
    T_CHECK(has_text(p, "P5"));
    T_CHECK(snap_get(sn, 5) == p);
    ps_snapshot_close(sn);
    T_EQ(ps_test_chain_len(s, 5), 1); /* still the cached disk version */
    T_EQ(stats().frames_retired, 0);

    const u64 misses = stats().cache_misses;
    op_check(3, 5, "P5");
    T_EQ(stats().cache_misses, misses);
    free_with(4, 5); /* nothing registered: the resident version goes at commit */
    T_EQ(ps_test_chain_len(s, 5), 1);
    T_OK(ps_checkpoint(s));
    T_CHECK(!ps_test_has_entry(s, 5));
    close_store();
}

/* With the pin gone, abort can drop the disk node too. */
static void test_abort_frees_unpinned_base(void)
{
    disk_pages(64, 5);
    const u32 free0 = stats().frames_free;
    T_OK(ps_op_begin(s, 2));
    T_OK(ps_free(s, 5));
    ps_snapshot *sn = snap_open();
    snap_check(sn, 5, "P5");
    ps_snapshot_unpin(sn, 5);
    ps_op_abort(s);
    T_CHECK(!ps_test_has_entry(s, 5));
    T_EQ(stats().frames_free, free0);
    snap_check(sn, 5, "P5"); /* from disk again */
    ps_snapshot_close(sn);
    close_store();
}

/* A snapshot thread that reads one page and reports the pointer. */
typedef struct reader {
    pthread_t t;
    ps_snapshot *sn;
    u32 id;
    const ps_page *p;
    int rv;
} reader;

static void *reader_main(void *arg)
{
    reader *r = arg;
    r->rv = ps_snapshot_read(r->sn, r->id, &r->p);
    return NULL;
}

static void reader_spawn(reader *r, ps_snapshot *sn, u32 id)
{
    r->sn = sn;
    r->id = id;
    r->p = NULL;
    r->rv = -1;
    T_OK(pthread_create(&r->t, NULL, reader_main, r));
}

/*
 * Wait for read_page so the caller can race an operation with id marked
 * LOADING. Release the read with mb_hold_reads(mb, false).
 */
static void reader_start_loading(reader *r, ps_snapshot *sn, u32 id)
{
    mb_hold_reads(mb, true);
    reader_spawn(r, sn, id);
    mb_wait_held_reads(mb, 1);
}

static int reader_join(reader *r)
{
    T_OK(pthread_join(r->t, NULL));
    return r->rv;
}

/* Releases the held reads once n threads wait on id's LOADING. */
typedef struct releaser {
    pthread_t t;
    u32 id;
    u32 n;
} releaser;

static void *releaser_main(void *arg)
{
    const releaser *r = arg;
    while (ps_test_waiters(s, r->id) < r->n) {
        sleep_ms(1);
    }
    mb_hold_reads(mb, false);
    return NULL;
}

static void releaser_start(releaser *r, u32 id, u32 n)
{
    r->id = id;
    r->n = n;
    T_OK(pthread_create(&r->t, NULL, releaser_main, r));
}

static void releaser_join(releaser *r)
{
    T_OK(pthread_join(r->t, NULL));
}

/* Abort races a page-in of the disk node inserted by the open operation. */
static void test_abort_waits_for_loading(void)
{
    disk_pages(64, 5);
    T_OK(ps_op_begin(s, 2));
    T_OK(ps_free(s, 5));
    ps_snapshot *sn = snap_open();
    reader r;
    reader_start_loading(&r, sn, 5);
    releaser rel;
    releaser_start(&rel, 5, 1); /* releases once abort waits */
    ps_op_abort(s);
    releaser_join(&rel);
    T_OK(reader_join(&r));
    T_CHECK(has_text(r.p, "P5"));
    T_EQ(ps_test_chain_len(s, 5), 1);
    T_EQ(ps_test_pins(s, 5), 1);
    ps_snapshot_close(sn);
    T_EQ(stats().frames_retired, 0);
    op_check(3, 5, "P5");
    close_store();
}

/* The writer waits for a snapshot's page-in and then uses the loaded version. */
static void test_writer_waits_for_snapshot_loading(void)
{
    disk_pages(64, 5);
    ps_snapshot *sn = snap_open();
    reader r;
    reader_start_loading(&r, sn, 3);
    const u64 misses = stats().cache_misses;
    releaser rel;
    releaser_start(&rel, 3, 1); /* releases once ps_read waits */
    T_OK(ps_op_begin(s, 2));
    const ps_page *p = NULL;
    T_OK(ps_read(s, 3, &p));
    releaser_join(&rel);
    T_OK(reader_join(&r));
    T_CHECK(p == r.p); /* one load, one version */
    T_EQ(stats().cache_misses, misses + 1);
    T_EQ(ps_test_pins(s, 3), 2);
    write_text(3, "W");
    ps_op_commit(s);
    T_CHECK(has_text(r.p, "P3"));
    T_EQ(stats().frames_retired, 1);
    ps_snapshot_close(sn);
    T_EQ(stats().frames_retired, 0);
    op_check(3, 3, "W");
    close_store();
}

/* The two readers share a page-in and both pin the same frame. */
static void test_two_snapshots_one_load(void)
{
    disk_pages(64, 5);
    ps_snapshot *s1 = snap_open();
    ps_snapshot *s2 = snap_open();
    const u64 misses = stats().cache_misses;
    reader r1;
    reader r2;
    reader_start_loading(&r1, s1, 4);
    releaser rel;
    releaser_start(&rel, 4, 1); /* releases once r2 waits */
    reader_spawn(&r2, s2, 4);
    releaser_join(&rel);
    T_OK(reader_join(&r1));
    T_OK(reader_join(&r2));
    T_CHECK(r1.p == r2.p);
    T_EQ(stats().cache_misses, misses + 1);
    T_EQ(ps_test_pins(s, 4), 2);
    ps_snapshot_close(s1);
    ps_snapshot_close(s2);
    T_EQ(ps_test_pins(s, 4), 0);
    close_store();
}

/* A cut-off while the snapshot is paging in a retired frameless version. */
static void test_cutoff_during_load(void)
{
    disk_pages(64, 5);
    const u32 free0 = stats().frames_free;
    ps_snapshot *sn = snap_open();
    free_with(2, 5); /* the lsn-0 node is retired for sn */
    reader r;
    reader_start_loading(&r, sn, 5);
    T_CHECK(ps_snap_cut_off_oldest(s)); /* the node is freed when the load's pin goes */
    mb_hold_reads(mb, false);
    T_ERR(reader_join(&r), ESTALE);
    T_EQ(stats().frames_retired, 0);
    T_EQ(stats().frames_free, free0);
    T_EQ(ps_test_chain_len(s, 5), 1); /* the marker */
    T_EQ(ps_test_pins(s, 5), 0);
    ps_snapshot_close(sn);
    T_EQ(stats().frames_retired, 0);
    close_store();
}

/* Commit retires the node before page-in can give it a frame. */
static void test_commit_during_load(void)
{
    disk_pages(64, 5);
    const u32 free0 = stats().frames_free;
    ps_snapshot *sn = snap_open();
    T_OK(ps_op_begin(s, 2));
    T_OK(ps_free(s, 5));
    reader r;
    reader_start_loading(&r, sn, 5);
    ps_op_commit(s);
    T_EQ(stats().frames_retired, 0); /* retired, but frameless */
    mb_hold_reads(mb, false);
    T_OK(reader_join(&r));
    T_CHECK(has_text(r.p, "P5"));
    T_EQ(stats().frames_retired, 1);
    ps_snapshot_close(sn);
    T_EQ(stats().frames_retired, 0);
    T_EQ(stats().frames_free, free0);
    close_store();
}

/* Cut-off removes the reader, but page-in still holds a temporary pin. */
static void test_commit_dead_during_load(void)
{
    disk_pages(64, 5);
    const u32 free0 = stats().frames_free;
    ps_snapshot *sn = snap_open();
    T_OK(ps_op_begin(s, 2));
    T_OK(ps_free(s, 5));
    reader r;
    reader_start_loading(&r, sn, 5);
    T_CHECK(ps_snap_cut_off_oldest(s));
    ps_op_commit(s); /* nobody registered: freed at the load's last unpin */
    mb_hold_reads(mb, false);
    T_ERR(reader_join(&r), ESTALE);
    T_EQ(stats().frames_retired, 0);
    T_EQ(stats().frames_free, free0);
    T_EQ(ps_test_chain_len(s, 5), 1);
    T_EQ(ps_test_pins(s, 5), 0);
    ps_snapshot_close(sn);
    close_store();
}

/* ps_alloc's load of a retired frameless version waits for a snapshot loading it. */
static void test_alloc_during_snapshot_load(void)
{
    disk_pages(64, 5);
    ps_snapshot *sn = snap_open();
    free_with(2, 5);
    reader r;
    reader_start_loading(&r, sn, 5);
    const u64 misses = stats().cache_misses;
    releaser rel;
    releaser_start(&rel, 5, 1); /* releases once ps_alloc waits */
    T_OK(ps_op_begin(s, 3));
    T_EQ(alloc_text("C"), 5);
    ps_op_commit(s);
    releaser_join(&rel);
    T_OK(reader_join(&r));
    T_EQ(stats().cache_misses, misses + 1); /* loaded once */
    T_OK(ps_checkpoint(s));
    T_CHECK(has_text((const ps_page *) mb_page(mb, 5), "C"));
    T_CHECK(has_text(r.p, "P5"));
    snap_check(sn, 5, "P5");
    T_EQ(stats().frames_retired, 1);
    ps_snapshot_close(sn);
    T_EQ(stats().frames_retired, 0);
    op_check(4, 5, "C");
    close_store();
}

/*
 * Check a too-new disk version in two paths. An empty chain loads a new
 * node. The chain under a free marker only learns its LSN at page-in.
 */
static void test_newer_disk_version_einval(void)
{
    open_store(64);
    T_EQ(alloc_with(10, "A"), 1);
    ps_snapshot *sn = snap_open();
    T_EQ(ps_snapshot_lsn(sn), 10);
    T_OK(ps_op_begin(s, 15));
    T_EQ(alloc_text("N2"), 2);
    T_EQ(alloc_text("N3"), 3);
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    T_CHECK(ps_test_evict(s, 2));
    T_CHECK(ps_test_evict(s, 3));
    T_CHECK(!ps_test_has_entry(s, 2));

    /* Empty chain: loaded, written at 15 > 10. */
    const ps_page *p = NULL;
    T_ERR(ps_snapshot_read(sn, 3, &p), EINVAL);
    T_ERR(ps_snapshot_read(sn, 3, &p), EINVAL); /* resident now */
    T_EQ(ps_test_pins(s, 3), 0);

    /* lsn-0 node under the free marker, kept for sn. */
    free_with(20, 2);
    T_EQ(ps_test_chain_len(s, 2), 2);
    T_ERR(ps_snapshot_read(sn, 2, &p), EINVAL);
    T_ERR(ps_snapshot_read(sn, 2, &p), EINVAL); /* its LSN is known now */
    T_EQ(ps_test_pins(s, 2), 0);
    T_EQ(stats().frames_retired, 1);
    snap_check(sn, 1, "A");

    ps_snapshot *s2 = snap_open();
    T_ERR(ps_snapshot_read(s2, 2, &p), EINVAL); /* freed at 20 */
    snap_check(s2, 3, "N3");
    ps_snapshot_close(s2);
    ps_snapshot_close(sn);
    T_EQ(stats().frames_retired, 0);
    T_EQ(ps_test_chain_len(s, 2), 1);
    close_store();
}

/* ---- concurrent readers and a writer ---- */

#define CR_PAGES 64u /* stable pages 1..CR_PAGES, always allocated */
#define CR_CHURN 32u /* ids above them that the writer allocates and frees */
#define CR_READERS 4

typedef struct cr_reader {
    pthread_t t;
    atomic_bool *stop;
    u64 rounds;
} cr_reader;

/* The fixture stamps the writer's LSN in the payload as well as the header. */
static u64 page_value(const ps_page *p)
{
    u64 v;
    memcpy(&v, (const u8 *) p + HDR, sizeof(v));
    return v;
}

static void set_value(ps_page *p, u64 v)
{
    memcpy((u8 *) p + HDR, &v, sizeof(v));
}

static void check_read(const ps_page *p, u32 id, u64 S)
{
    T_EQ(p->page_id, id);
    T_CHECK(p->lsn <= S);
    T_EQ(page_value(p), p->lsn);
}

static void *cr_main(void *arg)
{
    cr_reader *r = arg;
    while (!atomic_load(r->stop) || r->rounds == 0) {
        ps_snapshot *sn = NULL;
        T_OK(ps_snapshot_open(s, &sn));
        const u64 S = ps_snapshot_lsn(sn);
        for (u32 id = 1; id <= CR_PAGES; id++) {
            const ps_page *p = NULL;
            T_OK(ps_snapshot_read(sn, id, &p));
            check_read(p, id, S);
            const ps_page *q = NULL;
            T_OK(ps_snapshot_read(sn, id, &q));
            T_CHECK(p == q);
            if (id % 3 != 0) {
                ps_snapshot_unpin(sn, id);
            }
        }
        /*
         * A churn page may be unallocated at S: EINVAL while its free marker is in
         * memory, EBADMSG if it was never written. Check the version on success.
         */
        for (u32 id = CR_PAGES + 1; id <= CR_PAGES + CR_CHURN; id++) {
            const ps_page *p = NULL;
            const int rv = ps_snapshot_read(sn, id, &p);
            T_CHECK(rv == 0 || rv == EINVAL || rv == EBADMSG);
            if (rv == 0) {
                check_read(p, id, S);
                ps_snapshot_unpin(sn, id);
            }
        }
        ps_snapshot_close(sn);
        r->rounds++;
    }
    return NULL;
}

static u64 rng_next(u64 *x)
{
    *x = *x * 6364136223846793005ull + 1442695040888963407ull;
    return *x >> 33;
}

static void test_concurrent_readers(void)
{
    open_store(1024);
    T_OK(ps_op_begin(s, 1));
    for (u32 i = 1; i <= CR_PAGES; i++) {
        u32 id = 0;
        ps_page *p = NULL;
        T_OK(ps_alloc(s, &id, &p));
        T_EQ(id, i);
        set_value(p, 1);
    }
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    reopen(1024); /* readers and the writer page in concurrently */

    atomic_bool stop;
    atomic_init(&stop, false);
    cr_reader rs[CR_READERS];
    for (u32 i = 0; i < CR_READERS; i++) {
        rs[i].stop = &stop;
        rs[i].rounds = 0;
        T_OK(pthread_create(&rs[i].t, NULL, cr_main, &rs[i]));
    }
    bool live[CR_CHURN] = {false}; /* committed allocation state of the churn ids */
    const u64 nops = 300u * t_scale();
    u64 x = 12345;
    for (u64 lsn = 2; lsn < 2 + nops; lsn++) {
        bool next[CR_CHURN];
        memcpy(next, live, sizeof(next));
        T_OK(ps_op_begin(s, lsn));
        for (u32 k = 0; k < 3; k++) {
            const u32 id = 1 + (u32) (rng_next(&x) % CR_PAGES);
            ps_page *p = NULL;
            T_OK(ps_write(s, id, &p));
            set_value(p, lsn);
        }
        /* Free a live churn page (rewriting another first), or allocate one. */
        const u32 c = (u32) (rng_next(&x) % CR_CHURN);
        if (next[c] && rng_next(&x) % 2 == 0) {
            T_OK(ps_free(s, CR_PAGES + 1 + c));
            next[c] = false;
        } else {
            u32 n_live = 0;
            for (u32 i = 0; i < CR_CHURN; i++) {
                n_live += next[i] ? 1u : 0u;
            }
            if (n_live < CR_CHURN) {
                u32 id = 0;
                ps_page *p = NULL;
                T_OK(ps_alloc(s, &id, &p));
                T_CHECK(id > CR_PAGES && id <= CR_PAGES + CR_CHURN);
                T_CHECK(!next[id - CR_PAGES - 1]);
                next[id - CR_PAGES - 1] = true;
                set_value(p, lsn);
            }
        }
        if (lsn % 7 == 0) {
            ps_op_abort(s);
            continue;
        }
        ps_op_commit(s);
        memcpy(live, next, sizeof(live));
        if (lsn % 20 == 0) {
            T_OK(ps_checkpoint(s));
        }
    }
    atomic_store(&stop, true);
    for (u32 i = 0; i < CR_READERS; i++) {
        T_OK(pthread_join(rs[i].t, NULL));
        T_CHECK(rs[i].rounds > 0);
    }
    T_EQ(stats().frames_retired, 0);
    T_EQ(stats().frames_pinned, 0);
    T_EQ(stats().snapshots_open, 0);
    for (u32 id = 1; id <= CR_PAGES + CR_CHURN; id++) {
        T_EQ(ps_test_pins(s, id), 0);
        T_CHECK(ps_test_chain_len(s, id) <= 1);
    }
    close_store();
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_snapshot_sees_old);
    T_RUN(test_open_mid_operation);
    T_RUN(test_free_keeps_old);
    T_RUN(test_pin_survives_cut_off);
    T_RUN(test_same_id_one_pin);
    T_RUN(test_pin_cap);
    T_RUN(test_global_pin_limit);
    T_RUN(test_snapshot_bitmap_einval);
    T_RUN(test_not_yet_allocated_einval);
    T_RUN(test_checkpoint_respects_snapshot);
    T_RUN(test_alloc_loads_frameless_old);
    T_RUN(test_pagein_retired_counts);
    T_RUN(test_retired_freed_at_checkpoint_end);
    T_RUN(test_snapshot_lsn);
    T_RUN(test_pagein_and_close);
    T_RUN(test_abort_keeps_pinned_base);
    T_RUN(test_abort_frees_unpinned_base);
    T_RUN(test_abort_waits_for_loading);
    T_RUN(test_writer_waits_for_snapshot_loading);
    T_RUN(test_two_snapshots_one_load);
    T_RUN(test_cutoff_during_load);
    T_RUN(test_commit_during_load);
    T_RUN(test_commit_dead_during_load);
    T_RUN(test_alloc_during_snapshot_load);
    T_RUN(test_newer_disk_version_einval);
    T_RUN(test_concurrent_readers);
    return t_done();
}
