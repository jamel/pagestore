#include "allocator.h"
#include "mem_backend.h"
#include "ptable.h"
#include "store.h"
#include "test.h"

#include "pagestore/backend.h"
#include "pagestore/pagestore.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PS 4096u
#define HDR PS_PAGE_HEADER_SIZE

static mem_backend *mb;
static ps_store *s;

static void open_store(u32 frames)
{
    mb = mb_create();
    T_OK(mb_open(mb, frames, PS_CREATE, &s));
}

static void close_store(void)
{
    ps_close(s);
    s = NULL;
    mb_destroy(mb);
    mb = NULL;
}

static bool all_zero(const void *p, usize n)
{
    const u8 *b = p;
    for (usize i = 0; i < n; i++) {
        if (b[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Create a committed page in its own operation at lsn. */
static ps_page_id alloc_with(ps_lsn lsn, const char *text)
{
    T_OK(ps_op_begin(s, lsn));
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    memcpy((u8 *) p + HDR, text, strlen(text) + 1);
    ps_op_commit(s);
    return id;
}

static void check_text(ps_page_id id, const char *text)
{
    const ps_page *p = NULL;
    T_OK(ps_read(s, id, &p));
    T_STREQ((const char *) p + HDR, text);
}

struct ps_logger_ctx {
    atomic_int errors;
};

/* Formats every message so the sanitizers check its arguments. */
static void count_log(ps_logger_ctx *ctx, ps_log_level level, const char *fmt, va_list ap)
{
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    if (level == PS_LOG_LEVEL_ERROR) {
        atomic_fetch_add(&ctx->errors, 1);
    }
}

static void test_config_defaults(void)
{
    ps_config c;
    ps_config_init(&c);
    T_EQ(c.page_size, 0);
    T_EQ(c.cache_size, 64u << 20);
    T_CHECK(c.cache_memory == NULL);
    T_EQ(c.snapshot_pin_cap, 64);
    T_EQ(c.max_pinned_pct, 25);
    T_EQ(c.checkpoint_thread, true);
    T_EQ(c.checkpoint_interval_ms, 5000);
    T_EQ(c.checkpoint_dirty_pct, 40);
    T_CHECK(c.logger.ctx == NULL && c.logger.write == NULL && c.vfs == NULL);
}

static void test_bad_config(void)
{
    mb = mb_create();
    ps_backend be;
    ps_config cfg = mb_config(64);

    cfg.page_size = 2048;
    mb_backend(mb, &be);
    T_ERR(ps_open_backend(&be, &cfg, PS_CREATE, &s), EINVAL);
    cfg.page_size = 6000;
    T_ERR(ps_open_backend(&be, &cfg, PS_CREATE, &s), EINVAL);
    cfg = mb_config(64);
    cfg.max_pinned_pct = 101;
    T_ERR(ps_open_backend(&be, &cfg, PS_CREATE, &s), EINVAL);
    cfg = mb_config(64);
    cfg.max_pinned_pct = 0; /* snapshots could never pin a page */
    T_ERR(ps_open_backend(&be, &cfg, PS_CREATE, &s), EINVAL);
    cfg = mb_config(64);
    cfg.snapshot_pin_cap = 0;
    T_ERR(ps_open_backend(&be, &cfg, PS_CREATE, &s), EINVAL);

    T_ERR(mb_open(mb, 8, PS_CREATE, &s), EINVAL);
    T_ERR(mb_open(mb, 15, PS_CREATE, &s), EINVAL);
    mb_destroy(mb);

    mb = mb_create();
    T_ERR(mb_open(mb, 64, 0, &s), ENOENT);
    /* A seq 0 header is an interrupted format. */
    const ps_backend_info seq0 = {PS, 1, 0, PS_LSN_NONE};
    mb_set_info(mb, seq0);
    T_ERR(mb_open(mb, 64, 0, &s), ENOENT);
    /* PS_CREATE resumes by formatting from scratch. */
    T_OK(mb_open(mb, 64, PS_CREATE, &s));
    T_EQ(ps_committed_lsn(s).raw, 0);
    T_EQ(mb_info(mb).seq, 1);
    close_store();

    mb = mb_create();
    ps_backend_info info = {8192, 1, 1, PS_LSN_NONE};
    mb_set_info(mb, info);
    cfg = mb_config(64);
    mb_backend(mb, &be);
    T_ERR(ps_open_backend(&be, &cfg, 0, &s), EINVAL);
    mb_destroy(mb);
    mb = NULL;
}

static void test_format(void)
{
    open_store(64);
    T_EQ(ps_page_size(s), PS);
    T_EQ(ps_committed_lsn(s).raw, 0);
    T_EQ(ps_durable_lsn(s).raw, 0);
    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.num_groups, 1);
    T_EQ(st.allocated_pages, 0);
    T_EQ(st.frames_dirty, 0); /* format's checkpoint wrote bitmap page 0 */
    T_EQ(st.frames_free, 63);
    T_EQ(st.frames_pinned, 0);
    T_EQ(st.frames_evictable, 0);
    /* format's cancelled allocation left nothing */
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(1)));
    T_EQ(ps_test_chain_len(s, ps_page_id_from(0)), 1);
    T_EQ(ps_test_pins(s, ps_page_id_from(0)), 0);
    close_store();
}

static void test_alloc_write_read(void)
{
    open_store(64);
    T_OK(ps_op_begin(s, ps_lsn_from(1)));
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, 1);
    T_EQ(p->id.raw, 1);
    T_EQ(p->lsn.raw, 1);
    T_EQ(p->magic, 0);
    T_EQ(p->flags, 0);
    T_EQ(p->checksum, 0);
    T_CHECK(all_zero((const u8 *) p + HDR, PS - HDR));
    memcpy((u8 *) p + HDR, "hello", 6);
    /* ps_read and ps_write see the operation's own version. */
    const ps_page *rp = NULL;
    T_OK(ps_read(s, ps_page_id_from(1), &rp));
    T_CHECK(rp == p);
    ps_page *wp = NULL;
    T_OK(ps_write(s, ps_page_id_from(1), &wp));
    T_CHECK(wp == p);
    ps_op_commit(s);
    T_EQ(ps_committed_lsn(s).raw, 1);
    T_EQ(ps_durable_lsn(s).raw, 0);

    T_OK(ps_op_begin(s, ps_lsn_from(2)));
    check_text(ps_page_id_from(1), "hello");
    ps_op_commit(s);
    T_EQ(ps_committed_lsn(s).raw, 2);
    T_EQ(ps_test_pins(s, ps_page_id_from(1)), 0);
    close_store();
}

static void test_lsn_must_increase(void)
{
    open_store(64);
    T_OK(ps_op_begin(s, ps_lsn_from(1)));
    ps_op_commit(s);
    T_ERR(ps_op_begin(s, ps_lsn_from(1)), EINVAL);
    T_ERR(ps_op_begin(s, PS_LSN_NONE), EINVAL);
    /* Gaps are allowed. */
    T_OK(ps_op_begin(s, ps_lsn_from(10)));
    ps_op_commit(s);
    T_EQ(ps_committed_lsn(s).raw, 10);
    T_ERR(ps_op_begin(s, ps_lsn_from(9)), EINVAL);
    T_OK(ps_op_begin(s, ps_lsn_from(11)));
    ps_op_abort(s);
    T_EQ(ps_committed_lsn(s).raw, 10);
    T_OK(ps_op_begin(s, ps_lsn_from(11)));
    ps_op_commit(s);
    close_store();
}

/* A nested begin on the same thread returns EINVAL instead of deadlocking. */
static void test_nested_begin_einval(void)
{
    mb = mb_create();
    ps_logger_ctx lc;
    atomic_init(&lc.errors, 0);
    ps_config cfg = mb_config(64);
    cfg.logger = (ps_logger) {.max_level = PS_LOG_LEVEL_DEBUG, .ctx = &lc, .write = count_log};
    ps_backend be;
    mb_backend(mb, &be);
    T_OK(ps_open_backend(&be, &cfg, PS_CREATE, &s));

    T_OK(ps_op_begin(s, ps_lsn_from(1)));
    T_ERR(ps_op_begin(s, ps_lsn_from(2)), EINVAL);
    T_EQ(atomic_load(&lc.errors), 1);
    /* A newer LSN still cannot bypass the ownership check. */
    T_ERR(ps_op_begin(s, ps_lsn_from(1)), EINVAL);
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    ps_op_abort(s);
    T_EQ(ps_committed_lsn(s).raw, 0);
    T_OK(ps_op_begin(s, ps_lsn_from(2)));
    ps_op_commit(s);
    close_store();
}

typedef struct waiter_arg {
    atomic_bool committed;
    u64 begun_ms;
    bool saw_commit;
} waiter_arg;

static void *begin_waiter(void *p)
{
    waiter_arg *a = p;
    T_OK(ps_op_begin(s, ps_lsn_from(2)));
    a->begun_ms = t_now_ms();
    a->saw_commit = atomic_load(&a->committed) && ps_committed_lsn(s).raw == 1;
    ps_op_commit(s);
    return NULL;
}

static void test_begin_waits_for_other_thread(void)
{
    open_store(64);
    waiter_arg a;
    atomic_init(&a.committed, false);
    a.begun_ms = 0;
    a.saw_commit = false;

    T_OK(ps_op_begin(s, ps_lsn_from(1)));
    const u64 t0 = t_now_ms();
    pthread_t th;
    T_OK(pthread_create(&th, NULL, begin_waiter, &a));
    t_sleep_ms(100);
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    atomic_store(&a.committed, true);
    ps_op_commit(s);
    T_OK(pthread_join(th, NULL));
    T_CHECK(a.begun_ms >= t0 + 100);
    T_CHECK(a.saw_commit);
    T_EQ(ps_committed_lsn(s).raw, 2);
    close_store();
}

static void test_write_copies(void)
{
    open_store(64);
    T_EQ(alloc_with(ps_lsn_from(1), "A").raw, 1);

    T_OK(ps_op_begin(s, ps_lsn_from(2)));
    const ps_page *old = NULL;
    T_OK(ps_read(s, ps_page_id_from(1), &old));
    ps_page *neu = NULL;
    T_OK(ps_write(s, ps_page_id_from(1), &neu));
    T_CHECK(neu != old);
    T_EQ(neu->lsn.raw, 2);
    T_EQ(neu->id.raw, 1);
    T_EQ(neu->checksum, 0);
    T_EQ(old->lsn.raw, 1);
    memcpy((u8 *) neu + HDR, "B", 2);
    T_STREQ((const char *) old + HDR, "A");
    const ps_page *again = NULL;
    T_OK(ps_read(s, ps_page_id_from(1), &again));
    T_CHECK(again == neu);
    ps_page *w2 = NULL;
    T_OK(ps_write(s, ps_page_id_from(1), &w2));
    T_CHECK(w2 == neu);
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 2);
    ps_op_commit(s);

    /* The replaced version was freed at commit (no snapshots), and the operation's pin with it. */
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 1);
    T_EQ(ps_test_pins(s, ps_page_id_from(1)), 0);
    T_OK(ps_op_begin(s, ps_lsn_from(3)));
    check_text(ps_page_id_from(1), "B");
    ps_op_commit(s);
    close_store();
}

static void test_read_pins_once(void)
{
    open_store(64);
    alloc_with(ps_lsn_from(1), "A");
    struct ps_stats st0;
    ps_stats(s, &st0);

    T_OK(ps_op_begin(s, ps_lsn_from(2)));
    const ps_page *a = NULL;
    const ps_page *b = NULL;
    T_OK(ps_read(s, ps_page_id_from(1), &a));
    T_OK(ps_read(s, ps_page_id_from(1), &b));
    T_CHECK(a == b);
    T_EQ(ps_test_pins(s, ps_page_id_from(1)), 1);
    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.frames_pinned, 1);
    T_EQ(s->op.frames, 1);
    ps_op_abort(s);
    T_EQ(ps_test_pins(s, ps_page_id_from(1)), 0);
    ps_stats(s, &st);
    T_EQ(st.frames_pinned, 0);
    T_EQ(st.frames_free, st0.frames_free);
    close_store();
}

/* A version pinned by ps_read and replaced in the same operation is freed when it ends. */
static void test_read_then_replace(void)
{
    open_store(64);
    alloc_with(ps_lsn_from(1), "A");
    alloc_with(ps_lsn_from(2), "B");
    struct ps_stats st0;
    ps_stats(s, &st0);

    /* Abort drops the pins but leaves the committed versions. */
    T_OK(ps_op_begin(s, ps_lsn_from(3)));
    const ps_page *a = NULL;
    const ps_page *b = NULL;
    T_OK(ps_read(s, ps_page_id_from(1), &a));
    T_OK(ps_read(s, ps_page_id_from(2), &b));
    ps_page *w = NULL;
    T_OK(ps_write(s, ps_page_id_from(1), &w));
    T_OK(ps_free(s, ps_page_id_from(2)));
    T_EQ(ps_test_pins(s, ps_page_id_from(1)), 1);
    T_EQ(ps_test_pins(s, ps_page_id_from(2)), 1);
    ps_op_abort(s);
    T_EQ(ps_test_pins(s, ps_page_id_from(1)), 0);
    T_EQ(ps_test_pins(s, ps_page_id_from(2)), 0);
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 1);
    T_EQ(ps_test_chain_len(s, ps_page_id_from(2)), 1);

    /* Both old pointers stay valid up to commit, including for the freed page. */
    T_OK(ps_op_begin(s, ps_lsn_from(4)));
    T_OK(ps_read(s, ps_page_id_from(1), &a));
    T_OK(ps_read(s, ps_page_id_from(2), &b));
    T_OK(ps_write(s, ps_page_id_from(1), &w));
    memcpy((u8 *) w + HDR, "C", 2);
    T_OK(ps_free(s, ps_page_id_from(2)));
    T_STREQ((const char *) a + HDR, "A");
    T_STREQ((const char *) b + HDR, "B");
    ps_op_commit(s);
    T_EQ(ps_test_pins(s, ps_page_id_from(1)), 0);
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 1);
    T_EQ(ps_test_chain_len(s, ps_page_id_from(2)), 1); /* the marker */
    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.frames_pinned, 0);
    T_EQ(st.frames_free, st0.frames_free + 1); /* page 2's frame */
    T_EQ(st.allocated_pages, 1);

    T_OK(ps_op_begin(s, ps_lsn_from(5)));
    check_text(ps_page_id_from(1), "C");
    ps_op_commit(s);
    close_store();
}

static void test_abort_discards(void)
{
    open_store(64);
    alloc_with(ps_lsn_from(1), "A");
    T_OK(ps_op_begin(s, ps_lsn_from(2)));
    ps_page *p = NULL;
    T_OK(ps_write(s, ps_page_id_from(1), &p));
    memcpy((u8 *) p + HDR, "B", 2);
    ps_op_commit(s);
    struct ps_stats st0;
    ps_stats(s, &st0);

    T_OK(ps_op_begin(s, ps_lsn_from(3)));
    ps_page_id a = PS_PAGE_ID_NONE;
    ps_page_id b = PS_PAGE_ID_NONE;
    T_OK(ps_alloc(s, &a, &p));
    T_OK(ps_alloc(s, &b, &p));
    T_EQ(a.raw, 2);
    T_EQ(b.raw, 3);
    T_OK(ps_write(s, ps_page_id_from(1), &p));
    memcpy((u8 *) p + HDR, "C", 2);
    T_OK(ps_free(s, ps_page_id_from(1)));
    ps_op_abort(s);

    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.frames_free, st0.frames_free);
    T_EQ(st.frames_pinned, 0);
    T_EQ(st.allocated_pages, 1);
    T_CHECK(!ps_test_has_entry(s, a));
    T_CHECK(!ps_test_has_entry(s, b));
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 1);

    T_OK(ps_op_begin(s, ps_lsn_from(4)));
    check_text(ps_page_id_from(1), "B");
    ps_page_id a2 = PS_PAGE_ID_NONE;
    ps_page_id b2 = PS_PAGE_ID_NONE;
    T_OK(ps_alloc(s, &a2, &p));
    T_OK(ps_alloc(s, &b2, &p));
    T_EQ(a2.raw, a.raw);
    T_EQ(b2.raw, b.raw);
    ps_op_commit(s);
    ps_stats(s, &st);
    T_EQ(st.allocated_pages, 3);
    close_store();
}

static void test_freed_page_einval(void)
{
    open_store(64);
    alloc_with(ps_lsn_from(1), "A");
    T_OK(ps_op_begin(s, ps_lsn_from(2)));
    T_OK(ps_free(s, ps_page_id_from(1)));
    /* Within the freeing operation too. */
    const ps_page *rp = NULL;
    ps_page *wp = NULL;
    T_ERR(ps_read(s, ps_page_id_from(1), &rp), EINVAL);
    T_ERR(ps_write(s, ps_page_id_from(1), &wp), EINVAL);
    T_ERR(ps_free(s, ps_page_id_from(1)), EINVAL);
    ps_op_commit(s);
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 1); /* the marker, until a checkpoint */

    T_OK(ps_op_begin(s, ps_lsn_from(3)));
    T_ERR(ps_read(s, ps_page_id_from(1), &rp), EINVAL);
    T_ERR(ps_write(s, ps_page_id_from(1), &wp), EINVAL);
    T_ERR(ps_free(s, ps_page_id_from(1)), EINVAL);
    T_ERR(ps_read(s, ps_page_id_from(5), &rp), EINVAL);
    T_ERR(ps_write(s, ps_page_id_from(5), &wp), EINVAL);
    T_ERR(ps_free(s, ps_page_id_from(5)), EINVAL);
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(5)));
    /* Commit makes the freed ID eligible for reuse. */
    ps_page_id id = PS_PAGE_ID_NONE;
    T_OK(ps_alloc(s, &id, &wp));
    T_EQ(id.raw, 1);
    T_EQ(wp->lsn.raw, 3);
    T_CHECK(all_zero((const u8 *) wp + HDR, PS - HDR));
    ps_op_commit(s);
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 1); /* the marker was replaced and freed */
    close_store();
}

static void test_free_in_op(void)
{
    open_store(64);
    alloc_with(ps_lsn_from(1), "A");
    alloc_with(ps_lsn_from(2), "B");
    struct ps_stats st0;
    ps_stats(s, &st0);

    T_OK(ps_op_begin(s, ps_lsn_from(3)));
    /* Allocated and freed in the operation: cancelled, nothing remains. */
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, 3);
    T_OK(ps_free(s, id));
    T_CHECK(!ps_test_has_entry(s, id));
    /* Written then freed: the copy becomes the marker. */
    T_OK(ps_write(s, ps_page_id_from(1), &p));
    T_OK(ps_free(s, ps_page_id_from(1)));
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 2);
    /* The spare for group 0, and page 1's dirty version under the marker. */
    T_EQ(s->op.frames, 2);
    /* The cursor does not move back: the next allocation is 4. */
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, 4);
    ps_op_commit(s);

    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.allocated_pages, 2); /* 2 and 4 */
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 1);
    T_EQ(st.frames_free, st0.frames_free); /* -1 for page 1, +1 for page 4 */

    T_OK(ps_op_begin(s, ps_lsn_from(4)));
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, 1);
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, 3);
    ps_op_commit(s);
    close_store();
}

static void test_bitmap_ids_einval(void)
{
    open_store(64);
    alloc_with(ps_lsn_from(1), "A");
    T_OK(ps_op_begin(s, ps_lsn_from(2)));
    const ps_page *rp = NULL;
    ps_page *wp = NULL;
    T_ERR(ps_read(s, ps_page_id_from(0), &rp), EINVAL);
    T_ERR(ps_write(s, ps_page_id_from(0), &wp), EINVAL);
    T_ERR(ps_free(s, ps_page_id_from(0)), EINVAL);
    const ps_page_id g1 = ps_page_id_from(ps_group_size(PS));
    T_EQ(g1.raw, 32576);
    T_ERR(ps_read(s, g1, &rp), EINVAL);
    T_ERR(ps_write(s, g1, &wp), EINVAL);
    T_ERR(ps_free(s, g1), EINVAL);
    /* in a group that does not exist */
    T_ERR(ps_read(s, ps_page_id_from(g1.raw + 1), &rp), EINVAL);
    T_ERR(ps_free(s, ps_page_id_from(g1.raw + 1)), EINVAL);
    T_ERR(ps_read(s, ps_page_id_from(UINT32_MAX), &rp), EINVAL);
    T_ERR(ps_free(s, ps_page_id_from(UINT32_MAX)), EINVAL);
    check_text(ps_page_id_from(1), "A");
    ps_op_commit(s);
    close_store();
}

static void test_outside_op_einval(void)
{
    open_store(64);
    alloc_with(ps_lsn_from(1), "A");
    const ps_page *rp = NULL;
    ps_page *wp = NULL;
    ps_page_id id = PS_PAGE_ID_NONE;
    T_ERR(ps_read(s, ps_page_id_from(1), &rp), EINVAL);
    T_ERR(ps_write(s, ps_page_id_from(1), &wp), EINVAL);
    T_ERR(ps_alloc(s, &id, &wp), EINVAL);
    T_ERR(ps_free(s, ps_page_id_from(1)), EINVAL);
    T_EQ(ps_committed_lsn(s).raw, 1);
    close_store();
}

/* Seal a fixture page so it can be read back from the backend. */
static void make_page(u8 *buf, ps_page_id id, ps_lsn lsn, const char *text)
{
    memset(buf, 0, PS);
    ps_page *p = (ps_page *) buf;
    p->magic = 0x1234;
    p->id = id;
    p->lsn = lsn;
    strcpy((char *) buf + HDR, text);
    mb_seal_page(buf, PS);
}

/* Seed three pages at LSN 9. Page 2 has the wrong ID and page 3 a bad CRC. */
static void setup_pagein_store(void)
{
    mb = mb_create();
    ps_backend_info info = {PS, 1, 1, ps_lsn_from(9)};
    mb_set_info(mb, info);
    u8 *buf = t_calloc(1, PS);
    ps_page *p = (ps_page *) buf;
    p->magic = PS_BITMAP_MAGIC;
    p->id = ps_page_id_from(0);
    p->flags = PS_PAGE_FLAG_BITMAP;
    p->lsn = ps_lsn_from(9);
    buf[HDR] = 0x0F; /* bits 0..3 */
    mb_seal_page(buf, PS);
    mb_put(mb, ps_page_id_from(0), buf);

    make_page(buf, ps_page_id_from(1), ps_lsn_from(5), "one");
    mb_put(mb, ps_page_id_from(1), buf);
    make_page(buf, ps_page_id_from(7), ps_lsn_from(6), "two");
    mb_put(mb, ps_page_id_from(2), buf);
    make_page(buf, ps_page_id_from(3), ps_lsn_from(7), "three");
    buf[100] ^= 1;
    mb_put(mb, ps_page_id_from(3), buf);
    free(buf);
}

static void test_pagein_checks(void)
{
    setup_pagein_store();
    T_OK(mb_open(mb, 64, 0, &s));
    T_EQ(ps_committed_lsn(s).raw, 9);
    T_EQ(ps_durable_lsn(s).raw, 9);
    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.allocated_pages, 3);
    const u64 misses0 = st.cache_misses;
    T_EQ(misses0, 1); /* the bitmap page */

    T_OK(ps_op_begin(s, ps_lsn_from(10)));
    const ps_page *p = NULL;
    T_OK(ps_read(s, ps_page_id_from(1), &p));
    T_STREQ((const char *) p + HDR, "one");
    T_EQ(p->checksum, 0);
    T_EQ(p->lsn.raw, 5);
    T_EQ(p->magic, 0x1234);
    T_ERR(ps_read(s, ps_page_id_from(2), &p), EBADMSG);
    T_ERR(ps_read(s, ps_page_id_from(3), &p), EBADMSG);
    ps_page *wp = NULL;
    T_ERR(ps_write(s, ps_page_id_from(3), &wp), EBADMSG);
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(2)));
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(3)));
    ps_stats(s, &st);
    T_EQ(st.cache_misses, misses0 + 4);
    ps_op_abort(s);

    T_OK(ps_op_begin(s, ps_lsn_from(10)));
    T_OK(ps_read(s, ps_page_id_from(1), &p));
    T_STREQ((const char *) p + HDR, "one");
    ps_stats(s, &st);
    T_EQ(st.cache_misses, misses0 + 4); /* resident: a hit */
    T_EQ(st.cache_hits, 1);
    ps_page_id id = PS_PAGE_ID_NONE;
    T_OK(ps_alloc(s, &id, &wp));
    T_EQ(id.raw, 4);
    ps_op_commit(s);
    T_EQ(ps_committed_lsn(s).raw, 10);
    close_store();
}

static void test_pagein_read_error(void)
{
    setup_pagein_store();
    T_OK(mb_open(mb, 64, 0, &s));
    T_OK(ps_op_begin(s, ps_lsn_from(10)));
    mb_fail(mb, MB_READ, EIO, 1);
    const ps_page *p = NULL;
    T_ERR(ps_read(s, ps_page_id_from(1), &p), EIO);
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(1)));
    T_OK(ps_read(s, ps_page_id_from(1), &p));
    ps_op_commit(s);
    close_store();

    setup_pagein_store();
    u8 *buf = t_calloc(1, PS);
    memcpy(buf, mb_page(mb, ps_page_id_from(0)), PS);
    buf[HDR + 1] ^= 1;
    mb_put(mb, ps_page_id_from(0), buf);
    free(buf);
    T_ERR(mb_open(mb, 64, 0, &s), EBADMSG);
    mb_destroy(mb);
    mb = NULL;
}

/* Free can install a marker without reading the old data page. */
static void test_free_unloaded(void)
{
    setup_pagein_store();
    T_OK(mb_open(mb, 64, 0, &s));
    struct ps_stats st0;
    ps_stats(s, &st0);
    T_OK(ps_op_begin(s, ps_lsn_from(10)));
    T_OK(ps_free(s, ps_page_id_from(1)));
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 2);
    ps_op_abort(s);
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(1)));

    T_OK(ps_op_begin(s, ps_lsn_from(11)));
    T_OK(ps_free(s, ps_page_id_from(1)));
    ps_op_commit(s);
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 1); /* no snapshot: the disk node was freed */
    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.allocated_pages, 2);
    T_EQ(st.cache_misses, st0.cache_misses); /* free does not load */
    T_OK(ps_op_begin(s, ps_lsn_from(12)));
    const ps_page *p = NULL;
    T_ERR(ps_read(s, ps_page_id_from(1), &p), EINVAL);
    ps_op_commit(s);
    close_store();
}

static void test_cache_full_enobufs(void)
{
    open_store(16);
    struct ps_stats st0;
    ps_stats(s, &st0);
    T_OK(ps_op_begin(s, ps_lsn_from(1)));
    u32 n = 0;
    int rv = 0;
    for (;;) {
        ps_page_id id = PS_PAGE_ID_NONE;
        ps_page *p = NULL;
        rv = ps_alloc(s, &id, &p);
        if (rv != 0) {
            break;
        }
        n++;
        T_EQ(id.raw, n);
    }
    T_ERR(rv, ENOBUFS);
    /* The 11-frame budget fits ten user pages and the bitmap spare. */
    T_EQ(n, 10);
    ps_op_abort(s);
    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.frames_free, st0.frames_free);

    T_OK(ps_op_begin(s, ps_lsn_from(1)));
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, 1);
    ps_op_commit(s);
    close_store();
}

static void test_stats_basic(void)
{
    open_store(64);
    T_OK(ps_op_begin(s, ps_lsn_from(1)));
    for (u32 i = 0; i < 3; i++) {
        ps_page_id id = PS_PAGE_ID_NONE;
        ps_page *p = NULL;
        T_OK(ps_alloc(s, &id, &p));
    }
    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.allocated_pages, 0); /* not committed yet */
    ps_op_commit(s);
    ps_stats(s, &st);
    T_EQ(st.page_size, PS);
    T_EQ(st.num_groups, 1);
    T_EQ(st.allocated_pages, 3);
    T_EQ(st.committed_lsn.raw, 1);
    T_EQ(st.durable_lsn.raw, 0);
    T_EQ(st.frames_total, 64);
    T_EQ(st.frames_free, 64 - 4); /* bitmap + 3 pages; the spare was returned */
    T_EQ(st.frames_dirty, 4);
    T_EQ(st.frames_pinned, 0);
    T_EQ(st.frames_evictable, 0); /* dirty */
    T_EQ(st.frames_retired, 0);
    T_EQ(st.snapshots_open, 0);
    T_EQ(st.checkpoints_run, 1); /* format's */
    T_CHECK(!st.failed);
    T_CHECK(st.bookkeeping_bytes > 0);
    close_store();
}

static void test_clean_page_evictable(void)
{
    setup_pagein_store();
    T_OK(mb_open(mb, 64, 0, &s));
    T_OK(ps_op_begin(s, ps_lsn_from(10)));
    const ps_page *p = NULL;
    T_OK(ps_read(s, ps_page_id_from(1), &p));
    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.frames_evictable, 0); /* pinned by the operation */
    ps_op_commit(s);
    ps_stats(s, &st);
    T_EQ(st.frames_evictable, 1); /* clean, on disk, unpinned; bitmap pages never are */
    T_OK(ps_op_begin(s, ps_lsn_from(11)));
    ps_page *wp = NULL;
    T_OK(ps_write(s, ps_page_id_from(1), &wp));
    ps_stats(s, &st);
    T_EQ(st.frames_evictable, 0); /* under the operation's copy */
    ps_op_commit(s);
    ps_stats(s, &st);
    T_EQ(st.frames_evictable, 0); /* the new head is dirty */
    close_store();
}

/* ---- groups beyond 0 ---- */

/* An existing store (durable 9) whose group 0 is full; pages 1..nuser are on disk. */
static void setup_full_group0(u32 nuser)
{
    mb = mb_create();
    ps_backend_info info = {PS, 1, 1, ps_lsn_from(9)};
    mb_set_info(mb, info);
    u8 *buf = t_calloc(1, PS);
    ps_page *p = (ps_page *) buf;
    p->magic = PS_BITMAP_MAGIC;
    p->id = ps_page_id_from(0);
    p->flags = PS_PAGE_FLAG_BITMAP;
    p->lsn = ps_lsn_from(9);
    memset(buf + HDR, 0xFF, PS - HDR);
    mb_seal_page(buf, PS);
    mb_put(mb, ps_page_id_from(0), buf);
    for (u32 i = 1; i <= nuser; i++) {
        make_page(buf, ps_page_id_from(i), ps_lsn_from(5), "x");
        mb_put(mb, ps_page_id_from(i), buf);
    }
    free(buf);
}

/* Returns the resident bitmap bytes after the page header. */
static const u8 *group_bits(u32 g, u64 *lsn)
{
    const ps_page_id id = ps_page_id_from(g * ps_group_size(PS));
    ps_latch(s->pt, id);
    const ps_entry *e = ps_entry_find(s->pt, id);
    T_CHECK(e != NULL && e->head != NULL && e->head->frame != PS_NO_FRAME);
    const ps_page *p = ps_node_page(s, e->head);
    T_EQ(p->id.raw, id.raw);
    T_EQ(p->flags, PS_PAGE_FLAG_BITMAP);
    T_EQ(p->magic, PS_BITMAP_MAGIC);
    T_EQ(p->lsn.raw, e->head->lsn.raw);
    *lsn = p->lsn.raw;
    ps_unlatch(s->pt, id);
    return (const u8 *) p + HDR;
}

static void test_new_group_commit(void)
{
    setup_full_group0(0);
    T_OK(mb_open(mb, 64, 0, &s));
    const u32 G = ps_group_size(PS);
    struct ps_stats st0;
    ps_stats(s, &st0);
    T_EQ(st0.allocated_pages, G - 1);

    T_OK(ps_op_begin(s, ps_lsn_from(10)));
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, G + 1);
    T_EQ(p->id.raw, G + 1);
    T_EQ(s->op.frames, 2); /* the page and the new bitmap page */
    T_CHECK(ps_test_has_entry(s, ps_page_id_from(G))); /* the group's reserved entry */
    T_EQ(ps_test_chain_len(s, ps_page_id_from(G)), 0);
    ps_op_commit(s);

    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.num_groups, 2);
    T_EQ(st.allocated_pages, G);
    T_EQ(st.frames_free, st0.frames_free - 2);
    T_EQ(st.frames_dirty, 2); /* bitmap 0 untouched */
    T_EQ(st.frames_pinned, 0);
    T_EQ(ps_test_chain_len(s, ps_page_id_from(G)), 1);
    T_EQ(ps_test_pins(s, ps_page_id_from(G)), 0);
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(2 * G))); /* the kit for group 2 was not taken */
    u64 lsn = 0;
    const u8 *bits = group_bits(1, &lsn);
    T_EQ(lsn, 10);
    T_EQ(bits[0], 0x03); /* bit 0 (itself) and G + 1 */
    T_CHECK(all_zero(bits + 1, PS - HDR - 1));
    T_EQ(group_bits(0, &lsn)[0], 0xFF);
    T_EQ(lsn, 9);

    /* The new group is an existing one now: spare taken and returned. */
    T_OK(ps_op_begin(s, ps_lsn_from(11)));
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, G + 2);
    T_OK(ps_free(s, ps_page_id_from(G + 1)));
    /* The page, group 1's spare and G + 1's dirty version under the marker. */
    T_EQ(s->op.frames, 3);
    ps_op_commit(s);
    ps_stats(s, &st);
    T_EQ(st.allocated_pages, G);
    T_EQ(st.frames_free, st0.frames_free - 2);
    bits = group_bits(1, &lsn);
    T_EQ(lsn, 11);
    T_EQ(bits[0], 0x05);

    T_OK(ps_op_begin(s, ps_lsn_from(12)));
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, G + 1);
    ps_op_abort(s);
    ps_stats(s, &st);
    T_EQ(st.frames_free, st0.frames_free - 2);
    close_store();
}

static void test_new_group_abort(void)
{
    setup_full_group0(0);
    T_OK(mb_open(mb, 64, 0, &s));
    const u32 G = ps_group_size(PS);
    struct ps_stats st0;
    ps_stats(s, &st0);

    T_OK(ps_op_begin(s, ps_lsn_from(10)));
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, G + 1);
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, G + 2);
    T_OK(ps_free(s, ps_page_id_from(G + 1)));
    T_CHECK(ps_test_has_entry(s, ps_page_id_from(2 * G))); /* reserve for a group 2 */
    ps_op_abort(s);

    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.num_groups, 1);
    T_EQ(st.allocated_pages, G - 1);
    T_EQ(st.frames_free, st0.frames_free);
    T_EQ(st.frames_dirty, 0);
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(G)));
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(G + 1)));
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(G + 2)));
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(2 * G)));

    T_OK(ps_op_begin(s, ps_lsn_from(10)));
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, G + 1);
    ps_op_commit(s);
    ps_stats(s, &st);
    T_EQ(st.num_groups, 2);
    close_store();
}

/*
 * Exhaust the 11-frame budget so the new bitmap takes the user page's frame.
 * Committing after the error checks that the operation is still consistent.
 */
static void test_new_group_frame_steal(void)
{
    setup_full_group0(10);
    T_OK(mb_open(mb, 16, 0, &s));
    const u32 G = ps_group_size(PS);
    const ps_page *rp = NULL;
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    struct ps_stats st;

    T_OK(ps_op_begin(s, ps_lsn_from(10)));
    for (u32 i = 1; i <= 10; i++) {
        T_OK(ps_read(s, ps_page_id_from(i), &rp));
    }
    ps_stats(s, &st);
    T_EQ(st.frames_free, 5);
    T_ERR(ps_alloc(s, &id, &p), ENOBUFS); /* group 1 created, its frame stolen */
    T_EQ(s->op.frames, 11);
    ps_stats(s, &st);
    T_EQ(st.frames_free, 4);
    T_ERR(ps_alloc(s, &id, &p), ENOBUFS); /* no room for a page at all */
    ps_op_abort(s);
    ps_stats(s, &st);
    T_EQ(st.frames_free, 5);
    T_EQ(st.frames_evictable, 10);
    T_EQ(st.frames_pinned, 0);
    T_EQ(st.num_groups, 1);
    T_EQ(st.allocated_pages, G - 1);
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(G)));
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(G + 1)));
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(2 * G)));

    T_OK(ps_op_begin(s, ps_lsn_from(11)));
    for (u32 i = 1; i <= 10; i++) {
        T_OK(ps_read(s, ps_page_id_from(i), &rp));
    }
    T_ERR(ps_alloc(s, &id, &p), ENOBUFS);
    ps_op_commit(s);
    ps_stats(s, &st);
    T_EQ(st.num_groups, 2);
    T_EQ(st.allocated_pages, G - 1);
    T_EQ(st.frames_free, 4);
    T_EQ(st.frames_pinned, 0);
    T_EQ(st.frames_dirty, 1); /* the new bitmap page */
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(G + 1)));
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(2 * G)));
    u64 lsn = 0;
    const u8 *bits = group_bits(1, &lsn);
    T_EQ(lsn, 11);
    T_EQ(bits[0], 0x01);
    T_CHECK(all_zero(bits + 1, PS - HDR - 1));
    close_store();
}

/* Cancelling all allocations in the operation leaves the bitmap clean. */
static void test_cancelled_only_bitmap_clean(void)
{
    setup_pagein_store();
    T_OK(mb_open(mb, 64, 0, &s));
    T_OK(ps_op_begin(s, ps_lsn_from(10)));
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, 4);
    T_OK(ps_free(s, ps_page_id_from(4)));
    ps_op_commit(s);
    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.frames_dirty, 0);
    T_EQ(st.allocated_pages, 3);
    u64 lsn = 0;
    T_EQ(group_bits(0, &lsn)[0], 0x0F);
    T_EQ(lsn, 9);
    T_EQ(ps_committed_lsn(s).raw, 10);
    close_store();
}

/* ---- operation ownership ---- */

typedef struct intruder_arg {
    int rv[4];
} intruder_arg;

static void *intruder(void *argp)
{
    intruder_arg *a = argp;
    const ps_page *rp = NULL;
    ps_page *wp = NULL;
    ps_page_id id = PS_PAGE_ID_NONE;
    a->rv[0] = ps_read(s, ps_page_id_from(1), &rp);
    a->rv[1] = ps_write(s, ps_page_id_from(1), &wp);
    a->rv[2] = ps_alloc(s, &id, &wp);
    a->rv[3] = ps_free(s, ps_page_id_from(1));
    return NULL;
}

static void test_non_owner_einval(void)
{
    mb = mb_create();
    ps_logger_ctx lc;
    atomic_init(&lc.errors, 0);
    ps_config cfg = mb_config(64);
    cfg.logger = (ps_logger) {.max_level = PS_LOG_LEVEL_DEBUG, .ctx = &lc, .write = count_log};
    ps_backend be;
    mb_backend(mb, &be);
    T_OK(ps_open_backend(&be, &cfg, PS_CREATE, &s));
    alloc_with(ps_lsn_from(1), "A");
    struct ps_stats st0;
    ps_stats(s, &st0);

    T_OK(ps_op_begin(s, ps_lsn_from(2)));
    intruder_arg a;
    pthread_t th;
    T_OK(pthread_create(&th, NULL, intruder, &a));
    T_OK(pthread_join(th, NULL));
    for (u32 i = 0; i < 4; i++) {
        T_ERR(a.rv[i], EINVAL);
    }
    T_EQ(atomic_load(&lc.errors), 4);
    T_EQ(s->op.frames, 0);
    T_EQ(s->op.npages, 0);
    check_text(ps_page_id_from(1), "A");
    ps_op_commit(s);
    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.allocated_pages, 1);
    T_EQ(st.frames_free, st0.frames_free);

    T_OK(pthread_create(&th, NULL, intruder, &a));
    T_OK(pthread_join(th, NULL));
    for (u32 i = 0; i < 4; i++) {
        T_ERR(a.rv[i], EINVAL);
    }
    T_EQ(atomic_load(&lc.errors), 8);
    close_store();
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_config_defaults);
    T_RUN(test_bad_config);
    T_RUN(test_format);
    T_RUN(test_alloc_write_read);
    T_RUN(test_lsn_must_increase);
    T_RUN(test_nested_begin_einval);
    T_RUN(test_begin_waits_for_other_thread);
    T_RUN(test_write_copies);
    T_RUN(test_read_pins_once);
    T_RUN(test_read_then_replace);
    T_RUN(test_abort_discards);
    T_RUN(test_freed_page_einval);
    T_RUN(test_free_in_op);
    T_RUN(test_bitmap_ids_einval);
    T_RUN(test_outside_op_einval);
    T_RUN(test_pagein_checks);
    T_RUN(test_pagein_read_error);
    T_RUN(test_free_unloaded);
    T_RUN(test_cache_full_enobufs);
    T_RUN(test_stats_basic);
    T_RUN(test_clean_page_evictable);
    T_RUN(test_new_group_commit);
    T_RUN(test_new_group_abort);
    T_RUN(test_new_group_frame_steal);
    T_RUN(test_cancelled_only_bitmap_clean);
    T_RUN(test_non_owner_einval);
    return t_done();
}
