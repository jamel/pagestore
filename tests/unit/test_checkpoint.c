#include "allocator.h"
#include "crc32c.h"
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

static void open_store(u32 frames)
{
    mb = mb_create();
    T_OK(mb_open(mb, frames, PS_CREATE, &s));
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

/* Use inside an open operation. alloc_with starts and commits one. */
static u32 alloc_text(const char *text)
{
    u32 id = 0;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    memcpy((u8 *) p + HDR, text, strlen(text) + 1);
    return id;
}

/* Create a committed page holding text in its own operation at lsn. */
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

static void check_text(u32 id, const char *text)
{
    const ps_page *p = NULL;
    T_OK(ps_read(s, id, &p));
    T_EQ(strcmp((const char *) p + HDR, text), 0);
    T_EQ(p->checksum, 0);
}

/* The durable image of id carries text. */
static void check_durable_text(u32 id, const char *text)
{
    const u8 *p = mb_page(mb, id);
    T_CHECK(p != NULL);
    T_EQ(strcmp((const char *) p + HDR, text), 0);
}

static bool durable_bit(u32 group_id, u32 bit)
{
    const u8 *p = mb_page(mb, group_id);
    T_CHECK(p != NULL);
    return (p[HDR + bit / 8] >> (bit % 8) & 1u) != 0;
}

/* ---- log hooks recorder ---- */

struct ps_log_hooks_ctx {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int flush_rv;
    u32 nflush;
    u32 ndone;
    u64 flush_lsn;
    u64 done_lsn;
    u64 count_at_flush; /* mb_ckpt_count when flush_log ran */
    u64 count_at_done;
    char order[32]; /* 'F' per flush_log, 'D' per checkpoint_done */
    u32 norder;
};

static void hooks_init(ps_log_hooks_ctx *h)
{
    memset(h, 0, sizeof(*h));
    pthread_mutex_init(&h->mu, NULL);
    pthread_cond_init(&h->cv, NULL);
}

static void hooks_destroy(ps_log_hooks_ctx *h)
{
    pthread_cond_destroy(&h->cv);
    pthread_mutex_destroy(&h->mu);
}

static int rec_flush(ps_log_hooks_ctx *h, u64 lsn)
{
    pthread_mutex_lock(&h->mu);
    h->nflush++;
    h->flush_lsn = lsn;
    h->count_at_flush = mb_ckpt_count(mb);
    if (h->norder < sizeof(h->order) - 1) {
        h->order[h->norder++] = 'F';
    }
    const int rv = h->flush_rv;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
    return rv;
}

static void rec_done(ps_log_hooks_ctx *h, u64 lsn)
{
    pthread_mutex_lock(&h->mu);
    h->ndone++;
    h->done_lsn = lsn;
    h->count_at_done = mb_ckpt_count(mb);
    if (h->norder < sizeof(h->order) - 1) {
        h->order[h->norder++] = 'D';
    }
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
}

static void set_hooks(ps_log_hooks_ctx *h)
{
    const ps_log_hooks hooks = {rec_flush, rec_done, h};
    ps_set_log_hooks(s, &hooks);
}

/* n counts all flush_log calls since the recorder was initialized. */
static void wait_flushes(ps_log_hooks_ctx *h, u32 n)
{
    pthread_mutex_lock(&h->mu);
    while (h->nflush < n) {
        pthread_cond_wait(&h->cv, &h->mu);
    }
    pthread_mutex_unlock(&h->mu);
}

static u32 nflush(ps_log_hooks_ctx *h)
{
    pthread_mutex_lock(&h->mu);
    const u32 n = h->nflush;
    pthread_mutex_unlock(&h->mu);
    return n;
}

static u32 ndone(ps_log_hooks_ctx *h)
{
    pthread_mutex_lock(&h->mu);
    const u32 n = h->ndone;
    pthread_mutex_unlock(&h->mu);
    return n;
}

/* ---- checkpoint thread helpers ---- */

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

static void bg_start(bg_ckpt *b)
{
    b->rv = -1;
    T_OK(pthread_create(&b->t, NULL, bg_ckpt_main, b));
}

static int bg_join(bg_ckpt *b)
{
    T_OK(pthread_join(b->t, NULL));
    return b->rv;
}

/* Pause backend writes and wait until flush_log shows the checkpoint has its LSN. */
static void bg_start_blocked(bg_ckpt *b, ps_log_hooks_ctx *h)
{
    const u32 n = nflush(h);
    mb_block_writes(mb, true);
    bg_start(b);
    wait_flushes(h, n + 1);
}

struct ps_log_ctx {
    atomic_int errors;
};

static void count_log(ps_log_ctx *ctx, int level, const char *msg)
{
    PS_UNUSED(msg);
    if (level == PS_LOG_ERROR) {
        atomic_fetch_add(&ctx->errors, 1);
    }
}

/* ---- tests ---- */

static void test_format_seq1(void)
{
    open_store(64);
    const ps_backend_info info = mb_info(mb);
    T_EQ(info.seq, 1);
    T_EQ(info.durable_lsn, 0);
    T_EQ(info.num_groups, 1);
    T_EQ(ps_durable_lsn(s), 0);
    const ps_page *p = (const ps_page *) mb_page(mb, 0);
    T_CHECK(p != NULL);
    T_EQ(p->page_id, 0);
    T_EQ(p->flags, PS_PAGE_FLAG_BITMAP);
    T_EQ(p->magic, PS_BITMAP_MAGIC);
    T_EQ(p->lsn, 0);
    const u8 *bits = (const u8 *) p + HDR;
    T_EQ(bits[0], 0x01);
    for (u32 i = 1; i < PS - HDR; i++) {
        T_EQ(bits[i], 0);
    }
    const struct ps_stats st = stats();
    T_EQ(st.frames_dirty, 0);
    T_EQ(st.checkpoints_run, 1);
    T_EQ(st.checkpoints_failed, 0);
    T_EQ(ps_test_chain_len(s, 0), 1);
    close_store();

    /* An interrupted format (a seq 0 header) is formatted again with PS_CREATE. */
    mb = mb_create();
    const ps_backend_info seq0 = {PS, 1, 0, 0};
    mb_set_info(mb, seq0);
    T_ERR(mb_open(mb, 64, 0, &s), ENOENT);
    T_OK(mb_open(mb, 64, PS_CREATE, &s));
    T_EQ(mb_info(mb).seq, 1);
    T_CHECK(mb_written_last(mb, 0));
    T_OK(ps_op_begin(s, 1));
    T_EQ(alloc_text("x"), 1);
    ps_op_commit(s);
    close_store();
}

static void test_reopen(void)
{
    open_store(64);
    T_OK(ps_op_begin(s, 1));
    T_EQ(alloc_text("one"), 1);
    T_EQ(alloc_text("two"), 2);
    T_EQ(alloc_text("three"), 3);
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    T_EQ(ps_durable_lsn(s), 1);
    reopen(64);
    T_EQ(ps_committed_lsn(s), 1);
    T_EQ(ps_durable_lsn(s), 1);
    T_EQ(stats().allocated_pages, 3);
    T_OK(ps_op_begin(s, 2));
    check_text(1, "one");
    check_text(2, "two");
    check_text(3, "three");
    T_EQ(alloc_text("four"), 4);
    ps_op_commit(s);
    close_store();
}

static void test_close_without_checkpoint_loses_ops(void)
{
    open_store(64);
    T_EQ(alloc_with(1, "a"), 1);
    T_OK(ps_checkpoint(s));
    T_OK(ps_op_begin(s, 2));
    write_text(1, "b");
    ps_op_commit(s);
    T_EQ(alloc_with(3, "c"), 2);
    reopen(64);
    T_EQ(ps_committed_lsn(s), 1);
    T_EQ(ps_durable_lsn(s), 1);
    T_OK(ps_op_begin(s, 2));
    check_text(1, "a");
    T_EQ(alloc_text("c"), 2);
    ps_op_commit(s);
    close_store();
}

static void test_skip_when_clean(void)
{
    open_store(64);
    ps_log_hooks_ctx h;
    hooks_init(&h);
    set_hooks(&h);
    alloc_with(1, "a");
    T_OK(ps_checkpoint(s));
    T_EQ(ndone(&h), 1);
    const u64 count = mb_ckpt_count(mb);
    const u64 run = stats().checkpoints_run;
    T_OK(ps_checkpoint(s));
    T_EQ(mb_ckpt_count(mb), count);
    T_EQ(stats().checkpoints_run, run);
    T_EQ(nflush(&h), 1);
    T_EQ(ndone(&h), 1);
    close_store();
    hooks_destroy(&h);
}

static void test_lsn_only_advance(void)
{
    open_store(64);
    alloc_with(1, "a");
    T_OK(ps_checkpoint(s));
    T_OK(ps_op_begin(s, 5));
    check_text(1, "a");
    ps_op_commit(s);
    T_EQ(stats().frames_dirty, 0);
    const u64 count = mb_ckpt_count(mb);
    T_OK(ps_checkpoint(s));
    T_EQ(ps_durable_lsn(s), 5);
    T_EQ(mb_info(mb).durable_lsn, 5);
    T_EQ(mb_ckpt_count(mb), count + 1);
    T_CHECK(!mb_written_last(mb, 0));
    T_CHECK(!mb_written_last(mb, 1));
    close_store();
}

static void test_freed_page_skipped(void)
{
    open_store(64);
    T_EQ(alloc_with(1, "a"), 1);
    T_OK(ps_op_begin(s, 2));
    T_OK(ps_free(s, 1));
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    T_CHECK(!mb_written_last(mb, 1));
    T_CHECK(mb_page(mb, 1) == NULL);
    T_CHECK(mb_written_last(mb, 0));
    T_CHECK(durable_bit(0, 0));
    T_CHECK(!durable_bit(0, 1));
    T_CHECK(!ps_test_has_entry(s, 1)); /* the lone on-disk marker was dropped */
    T_EQ(stats().frames_dirty, 0);
    close_store();
}

static void test_mid_operation_and_bitmap_copy(void)
{
    open_store(64);
    ps_log_hooks_ctx h;
    hooks_init(&h);
    set_hooks(&h);
    T_EQ(alloc_with(1, "a"), 1);
    T_OK(ps_op_begin(s, 2));
    T_EQ(alloc_text("b"), 2);

    bg_ckpt b;
    bg_start_blocked(&b, &h); /* L = 1, blocked writing */
    T_EQ(h.flush_lsn, 1);
    ps_op_commit(s);
    T_EQ(ps_test_chain_len(s, 0), 2); /* the bitmap was copied for the checkpoint */
    T_EQ(ps_committed_lsn(s), 2);
    mb_block_writes(mb, false);
    T_OK(bg_join(&b));

    T_EQ(ps_durable_lsn(s), 1);
    T_CHECK(durable_bit(0, 1));
    T_CHECK(!durable_bit(0, 2));
    T_CHECK(mb_page(mb, 2) == NULL);
    T_EQ(ps_test_chain_len(s, 0), 1); /* the copy kept for it is gone */
    T_EQ(stats().frames_dirty, 2); /* bitmap 0 and page 2, from op 2 */

    T_OK(ps_checkpoint(s));
    T_EQ(ps_durable_lsn(s), 2);
    T_CHECK(durable_bit(0, 1));
    T_CHECK(durable_bit(0, 2));
    check_durable_text(2, "b");
    T_EQ(ps_test_chain_len(s, 0), 1);
    T_EQ(stats().frames_dirty, 0);
    close_store();
    hooks_destroy(&h);
}

/* Replacing a page must leave the running checkpoint's version readable. */
static void test_write_during_checkpoint(void)
{
    open_store(64);
    ps_log_hooks_ctx h;
    hooks_init(&h);
    set_hooks(&h);
    T_EQ(alloc_with(1, "A"), 1);
    const u32 free0 = stats().frames_free;

    bg_ckpt b;
    bg_start_blocked(&b, &h); /* L = 1 */
    T_OK(ps_op_begin(s, 2));
    write_text(1, "B");
    ps_op_commit(s);
    T_EQ(ps_test_chain_len(s, 1), 2);
    T_EQ(stats().frames_retired, 1);
    mb_block_writes(mb, false);
    T_OK(bg_join(&b));

    check_durable_text(1, "A");
    T_EQ(ps_test_chain_len(s, 1), 1);
    T_EQ(stats().frames_retired, 0);
    T_EQ(stats().frames_free, free0); /* the old version's frame came back */
    T_EQ(stats().frames_dirty, 1);
    T_OK(ps_checkpoint(s));
    check_durable_text(1, "B");
    T_EQ(stats().frames_dirty, 0);
    T_OK(ps_op_begin(s, 3));
    check_text(1, "B");
    ps_op_commit(s);
    close_store();
    hooks_destroy(&h);
}

/* Checkpoint still sees the free marker if its ID is reallocated. */
static void test_realloc_during_checkpoint(void)
{
    open_store(64);
    ps_log_hooks_ctx h;
    hooks_init(&h);
    set_hooks(&h);
    T_EQ(alloc_with(1, "A"), 1);
    T_OK(ps_op_begin(s, 2));
    T_OK(ps_free(s, 1));
    ps_op_commit(s);

    bg_ckpt b;
    bg_start_blocked(&b, &h); /* L = 2: bitmap 0 written, page 1 skipped */
    T_OK(ps_op_begin(s, 3));
    T_EQ(alloc_text("C"), 1);
    ps_op_commit(s);
    T_EQ(ps_test_chain_len(s, 1), 2); /* the new version over the marker */
    mb_block_writes(mb, false);
    T_OK(bg_join(&b));

    T_CHECK(mb_page(mb, 1) == NULL);
    T_CHECK(!durable_bit(0, 1));
    T_EQ(ps_test_chain_len(s, 1), 1);
    T_OK(ps_checkpoint(s));
    T_CHECK(durable_bit(0, 1));
    check_durable_text(1, "C");
    T_EQ(stats().frames_dirty, 0);
    close_store();
    hooks_destroy(&h);
}

static void test_hooks_order_and_failure(void)
{
    open_store(64);
    ps_log_ctx lc;
    atomic_init(&lc.errors, 0);
    s->cfg.log = count_log; /* Count only messages after format. */
    s->cfg.log_ctx = &lc;
    ps_log_hooks_ctx h;
    hooks_init(&h);
    set_hooks(&h);

    alloc_with(1, "a");
    const u64 c0 = mb_ckpt_count(mb);
    T_OK(ps_checkpoint(s));
    T_EQ(h.nflush, 1);
    T_EQ(h.flush_lsn, 1);
    T_EQ(h.count_at_flush, c0); /* before the backend commit */
    T_EQ(h.ndone, 1);
    T_EQ(h.done_lsn, 1);
    T_EQ(h.count_at_done, c0 + 1); /* after it */
    T_EQ(strcmp(h.order, "FD"), 0);

    /* Cleared hooks are not called. */
    ps_set_log_hooks(s, NULL);
    alloc_with(2, "b");
    T_OK(ps_checkpoint(s));
    T_EQ(h.nflush, 1);
    T_EQ(h.ndone, 1);
    T_EQ(ps_durable_lsn(s), 2);

    /* flush_log failing is fatal. */
    set_hooks(&h);
    h.flush_rv = EIO;
    alloc_with(3, "c");
    const u64 c1 = mb_ckpt_count(mb);
    T_ERR(ps_checkpoint(s), EIO);
    T_EQ(h.nflush, 2);
    T_EQ(h.ndone, 1);
    T_EQ(mb_ckpt_count(mb), c1);
    T_EQ(ps_durable_lsn(s), 2);
    T_ERR(ps_op_begin(s, 4), EIO);
    struct ps_stats st = stats();
    T_CHECK(st.failed);
    T_EQ(st.checkpoints_failed, 1);
    T_CHECK(atomic_load(&lc.errors) > 0);
    T_ERR(ps_checkpoint(s), EIO);
    T_EQ(h.nflush, 2); /* a failed store does not start checkpoints */
    st = stats();
    T_EQ(st.checkpoints_failed, 1);
    close_store();
    hooks_destroy(&h);
}

/* Sets hooks from another thread. */
typedef struct setter {
    pthread_t t;
    atomic_bool done;
    ps_log_hooks_ctx *h;
} setter;

static void *setter_main(void *arg)
{
    setter *st = arg;
    set_hooks(st->h);
    atomic_store(&st->done, true);
    return NULL;
}

/* A running checkpoint keeps its original hooks until it finishes. */
static void test_hooks_serialized(void)
{
    open_store(64);
    ps_log_hooks_ctx a;
    ps_log_hooks_ctx bh;
    hooks_init(&a);
    hooks_init(&bh);
    set_hooks(&a);
    alloc_with(1, "a");
    bg_ckpt b;
    bg_start_blocked(&b, &a);

    setter st;
    atomic_init(&st.done, false);
    st.h = &bh;
    T_OK(pthread_create(&st.t, NULL, setter_main, &st));
    sleep_ms(50);
    T_CHECK(!atomic_load(&st.done)); /* waits for the checkpoint */
    mb_block_writes(mb, false);
    T_OK(bg_join(&b));
    T_OK(pthread_join(st.t, NULL));
    T_CHECK(atomic_load(&st.done));
    T_EQ(ndone(&a), 1); /* the running checkpoint kept its hooks */
    T_EQ(nflush(&bh), 0);
    T_EQ(ndone(&bh), 0);

    alloc_with(2, "b");
    T_OK(ps_checkpoint(s));
    T_EQ(nflush(&a), 1);
    T_EQ(nflush(&bh), 1);
    T_EQ(ndone(&bh), 1);
    close_store();
    hooks_destroy(&a);
    hooks_destroy(&bh);
}

static void test_nonfatal_merges_dirty(void)
{
    open_store(64);
    T_OK(ps_op_begin(s, 1));
    T_EQ(alloc_text("a"), 1);
    T_EQ(alloc_text("b"), 2);
    ps_op_commit(s);
    T_EQ(stats().frames_dirty, 3);
    const u64 count = mb_ckpt_count(mb);

    mb_fail(mb, MB_WRITE, ENOSPC, 1);
    T_ERR(ps_checkpoint(s), ENOSPC);
    struct ps_stats st = stats();
    T_CHECK(!st.failed);
    T_EQ(st.checkpoints_failed, 1);
    T_EQ(st.checkpoints_run, 1); /* format's */
    T_EQ(st.frames_dirty, 3);
    T_EQ(ps_durable_lsn(s), 0);
    T_EQ(mb_ckpt_count(mb), count);

    /* A non-fatal commit failure too. */
    mb_fail(mb, MB_COMMIT, EIO, 1);
    T_ERR(ps_checkpoint(s), EIO);
    st = stats();
    T_CHECK(!st.failed);
    T_EQ(st.checkpoints_failed, 2);
    T_EQ(st.frames_dirty, 3);

    T_OK(ps_op_begin(s, 2)); /* healthy */
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    T_CHECK(mb_written_last(mb, 0));
    T_CHECK(mb_written_last(mb, 1));
    T_CHECK(mb_written_last(mb, 2));
    check_durable_text(1, "a");
    check_durable_text(2, "b");
    T_EQ(ps_durable_lsn(s), 2);
    T_EQ(stats().frames_dirty, 0);
    T_EQ(stats().checkpoints_run, 2);
    close_store();
}

/* A failed checkpoint merges a page already dirty on the newer list. */
static void test_merge_back_both_lists(void)
{
    open_store(64);
    ps_log_hooks_ctx h;
    hooks_init(&h);
    set_hooks(&h);
    T_OK(ps_op_begin(s, 1));
    T_EQ(alloc_text("a"), 1);
    T_EQ(alloc_text("b"), 2);
    ps_op_commit(s);
    T_EQ(stats().frames_dirty, 3);

    mb_fail(mb, MB_WRITE, ENOSPC, 1);
    bg_ckpt b;
    bg_start_blocked(&b, &h);
    T_OK(ps_op_begin(s, 2));
    write_text(1, "B");
    ps_op_commit(s);
    T_EQ(stats().frames_dirty, 4); /* page 1 is on both lists */
    T_EQ(stats().frames_retired, 1);
    mb_block_writes(mb, false);
    T_ERR(bg_join(&b), ENOSPC);

    struct ps_stats st = stats();
    T_EQ(st.frames_dirty, 3);
    T_EQ(st.frames_retired, 0);
    T_EQ(st.checkpoints_failed, 1);
    T_CHECK(!st.failed);
    T_EQ(ps_test_chain_len(s, 1), 1);
    T_EQ(ps_test_chain_len(s, 0), 1);

    T_OK(ps_checkpoint(s));
    T_CHECK(mb_written_last(mb, 0));
    T_CHECK(mb_written_last(mb, 1));
    T_CHECK(mb_written_last(mb, 2));
    check_durable_text(1, "B");
    check_durable_text(2, "b");
    T_EQ(stats().frames_dirty, 0);
    T_EQ(ndone(&h), 1);
    close_store();
    hooks_destroy(&h);
}

static void test_fatal_failed_state(void)
{
    open_store(64);
    alloc_with(1, "a");
    const ps_backend_info before = mb_info(mb);
    mb_fail(mb, MB_COMMIT_FATAL, EIO, 1);
    T_ERR(ps_checkpoint(s), EIO);
    struct ps_stats st = stats();
    T_CHECK(st.failed);
    T_EQ(st.checkpoints_failed, 1);
    T_ERR(ps_op_begin(s, 2), EIO);
    T_ERR(ps_checkpoint(s), EIO);
    T_EQ(mb_info(mb).seq, before.seq);
    T_EQ(ps_durable_lsn(s), 0);
    close_store();
}

static void test_commit_completes_in_failed_state(void)
{
    open_store(64);
    alloc_with(1, "a");
    alloc_with(2, "b");
    T_OK(ps_op_begin(s, 3));
    const u32 id = alloc_text("c");
    T_EQ(id, 3);
    mb_fail(mb, MB_COMMIT_FATAL, EIO, 1);
    bg_ckpt b;
    bg_start(&b);
    T_ERR(bg_join(&b), EIO);
    ps_page *p = NULL;
    T_ERR(ps_write(s, 1, &p), EIO);
    u32 id2 = 0;
    T_ERR(ps_alloc(s, &id2, &p), EIO);
    const ps_page *rp = NULL;
    T_ERR(ps_read(s, 1, &rp), EIO);
    T_ERR(ps_free(s, 1), EIO);
    ps_op_commit(s);
    T_EQ(ps_committed_lsn(s), 3);
    T_EQ(stats().allocated_pages, 3);
    T_CHECK(stats().failed);
    T_ERR(ps_op_begin(s, 4), EIO);
    close_store();
}

static void test_checksums(void)
{
    open_store(64);
    T_OK(ps_op_begin(s, 1));
    for (u32 i = 1; i <= 5; i++) {
        char text[16];
        snprintf(text, sizeof(text), "page %u", i);
        T_EQ(alloc_text(text), i);
    }
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    T_OK(ps_op_begin(s, 2));
    for (u32 id = 0; id <= 5; id++) {
        const u8 *d = mb_page(mb, id);
        T_CHECK(d != NULL);
        u8 buf[PS];
        memcpy(buf, d, PS);
        ps_page *dp = (ps_page *) buf;
        const u32 stored = dp->checksum;
        dp->checksum = 0;
        T_EQ(ps_crc32c(0, buf, PS), stored);
        T_NE(stored, 0);
        T_EQ(dp->page_id, id);
        if (id != 0) {
            const ps_page *p = NULL;
            T_OK(ps_read(s, id, &p));
            T_EQ(p->checksum, 0); /* the checkpoint never writes into frames */
            T_MEMEQ(p, buf, PS);
        }
    }
    ps_op_commit(s);
    close_store();
}

/* Fill most of group 0 in the fixture so a few allocations trigger growth. */
static void test_group_growth_persists(void)
{
    const u32 G = ps_group_size(PS);
    mb = mb_create();
    const ps_backend_info info = {PS, 1, 1, 9};
    mb_set_info(mb, info);
    u8 buf[PS];
    memset(buf, 0, PS);
    ps_page *bp = (ps_page *) buf;
    bp->magic = PS_BITMAP_MAGIC;
    bp->flags = PS_PAGE_FLAG_BITMAP;
    bp->lsn = 9;
    for (u32 bit = 0; bit < G - 10; bit++) {
        buf[HDR + bit / 8] |= (u8) (1u << (bit % 8));
    }
    mb_seal_page(buf, PS);
    mb_put(mb, 0, buf);
    T_OK(mb_open(mb, 64, 0, &s));
    T_EQ(stats().allocated_pages, G - 11);

    u64 lsn = 10;
    for (u32 chunk = 0; chunk < 2; chunk++) {
        T_OK(ps_op_begin(s, lsn++));
        for (u32 i = 0; i < 5; i++) {
            T_EQ(alloc_text("g0"), G - 10 + chunk * 5 + i);
        }
        ps_op_commit(s);
        T_OK(ps_checkpoint(s));
    }
    T_EQ(mb_info(mb).num_groups, 1);
    T_OK(ps_op_begin(s, lsn++));
    T_EQ(alloc_text("g1"), G + 1);
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    T_EQ(mb_info(mb).num_groups, 2);
    const ps_page *g1 = (const ps_page *) mb_page(mb, G);
    T_CHECK(g1 != NULL);
    T_EQ(g1->flags, PS_PAGE_FLAG_BITMAP);
    T_EQ(g1->page_id, G);
    T_EQ(((const u8 *) g1)[HDR], 0x03);
    check_durable_text(G + 1, "g1");

    reopen(64);
    struct ps_stats st = stats();
    T_EQ(st.num_groups, 2);
    T_EQ(st.allocated_pages, G);
    T_OK(ps_op_begin(s, lsn++));
    check_text(G + 1, "g1");
    check_text(G - 1, "g0");
    T_EQ(alloc_text("next"), G + 2);
    ps_op_commit(s);
    close_store();
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_format_seq1);
    T_RUN(test_reopen);
    T_RUN(test_close_without_checkpoint_loses_ops);
    T_RUN(test_skip_when_clean);
    T_RUN(test_lsn_only_advance);
    T_RUN(test_freed_page_skipped);
    T_RUN(test_mid_operation_and_bitmap_copy);
    T_RUN(test_write_during_checkpoint);
    T_RUN(test_realloc_during_checkpoint);
    T_RUN(test_hooks_order_and_failure);
    T_RUN(test_hooks_serialized);
    T_RUN(test_nonfatal_merges_dirty);
    T_RUN(test_merge_back_both_lists);
    T_RUN(test_fatal_failed_state);
    T_RUN(test_commit_completes_in_failed_state);
    T_RUN(test_checksums);
    T_RUN(test_group_growth_persists);
    return t_done();
}
