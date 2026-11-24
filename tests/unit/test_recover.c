#include "fault_vfs.h"
#include "test.h"

#include "pagestore/bytes.h"
#include "pagestore/pagestore.h"
#include "pagestore/wal.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PS 4096u
#define HDR PS_PAGE_HEADER_SIZE
#define SEG ((u64) 64 * 1024)
#define MAXREC (16u * 1024u)
#define REC_LEN 3000u /* about 21 records per segment */
#define NREC 50u
#define CKPT_LSN 20u
#define BODY 64u

static fault_vfs *fv;

static ps_config store_config(bool fault)
{
    ps_config cfg;
    ps_config_init(&cfg);
    cfg.page_size = PS;
    cfg.cache_size = (usize) 256 * PS;
    cfg.checkpoint_thread = false;
    cfg.vfs = fault ? fault_vfs_get(fv) : NULL;
    return cfg;
}

static ps_wal_config wal_config(void)
{
    ps_wal_config c;
    ps_wal_config_init(&c);
    c.segment_size = SEG;
    c.max_record_size = MAXREC;
    return c;
}

static ps_store *open_store(u32 flags)
{
    const ps_config cfg = store_config(true);
    ps_store *s = NULL;
    T_OK(ps_open("/s", &cfg, flags, &s));
    return s;
}

static ps_wal *open_wal(ps_store *s)
{
    const ps_wal_config c = wal_config();
    ps_wal *w = NULL;
    T_OK(ps_wal_open(s, NULL, &c, &w));
    return w;
}

static void close_both(ps_store *s, ps_wal *w)
{
    ps_close(s);
    ps_wal_close(w);
}

static u8 fill_of(u64 lsn)
{
    return (u8) (lsn * 29u + 3u);
}

/* The record carries the expected page ID so replay also tests allocator determinism. */
static void encode_rec(u8 *buf, ps_page_id id, u8 fill)
{
    memset(buf, 0, REC_LEN);
    ps_put_le32(buf, id.raw);
    buf[4] = fill;
    for (u32 i = 5; i < REC_LEN; i++) {
        buf[i] = (u8) (i ^ fill);
    }
}

/* Flush the log before committing the allocation. Returns its page ID. */
static ps_page_id logged_op(ps_store *s, ps_wal *w, u64 lsn)
{
    T_OK(ps_op_begin(s, ps_lsn_from(lsn)));
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    memset((u8 *) p + HDR, fill_of(lsn), BODY);
    u8 rec[REC_LEN];
    encode_rec(rec, id, fill_of(lsn));
    T_OK(ps_wal_append(w, ps_lsn_from(lsn), rec, sizeof(rec)));
    T_OK(ps_wal_flush(w, ps_lsn_from(lsn)));
    ps_op_commit(s);
    return id;
}

/* ---- replay callback ---- */

typedef enum apply_mode {
    APPLY_OK,
    APPLY_NO_OP,
    APPLY_NO_COMMIT, /* leaves an operation open */
    APPLY_FAIL,
    APPLY_FAIL_OPEN, /* reports an error with an operation open */
    APPLY_WRONG_LSN,
    APPLY_CKPT_THEN_FAIL
} apply_mode;

struct ps_apply_ctx {
    apply_mode mode;
    u64 fail_lsn;
    u64 ckpt_lsn;
    u64 seen[NREC + 1];
    u32 nseen;
};

static int apply_rec(ps_apply_ctx *ctx, ps_store *s, ps_lsn lsn, const void *rec, usize len)
{
    T_EQ(len, REC_LEN);
    T_CHECK(ctx->nseen < NREC + 1);
    ctx->seen[ctx->nseen++] = lsn.raw;
    const u8 *r = rec;
    const ps_page_id want_id = ps_page_id_from(ps_get_le32(r));
    const u8 fill = r[4];
    T_EQ(fill, fill_of(lsn.raw));
    if (ctx->mode == APPLY_NO_OP) {
        return 0;
    }
    if ((ctx->mode == APPLY_FAIL || ctx->mode == APPLY_CKPT_THEN_FAIL) &&
        lsn.raw == ctx->fail_lsn) {
        return EPROTO;
    }
    const ps_lsn op_lsn = ctx->mode == APPLY_WRONG_LSN ? ps_lsn_next(lsn) : lsn;
    int rv = ps_op_begin(s, op_lsn);
    if (rv != 0) {
        return rv;
    }
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    rv = ps_alloc(s, &id, &p);
    if (rv == 0 && !ps_page_id_eq(id, want_id)) {
        rv = EBADMSG;
    }
    if (rv != 0) {
        ps_op_abort(s);
        return rv;
    }
    memset((u8 *) p + HDR, fill, BODY);
    if (ctx->mode == APPLY_NO_COMMIT) {
        return 0;
    }
    if (ctx->mode == APPLY_FAIL_OPEN && lsn.raw == ctx->fail_lsn) {
        return EPROTO;
    }
    ps_op_commit(s);
    if (ctx->mode == APPLY_CKPT_THEN_FAIL && lsn.raw == ctx->ckpt_lsn) {
        T_OK(ps_checkpoint(s));
        T_EQ(ps_durable_lsn(s).raw, lsn.raw);
    }
    return 0;
}

static void ctx_init(ps_apply_ctx *ctx, apply_mode mode)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->mode = mode;
}

/* ids is indexed by record LSN, starting at 1. */
static void check_pages(ps_store *s, const ps_page_id *ids, u64 last)
{
    ps_snapshot *snap = NULL;
    T_OK(ps_snapshot_open(s, &snap));
    for (u64 lsn = 1; lsn <= last; lsn++) {
        const ps_page *p = NULL;
        T_OK(ps_snapshot_read(snap, ids[lsn], &p));
        const u8 *b = (const u8 *) p + HDR;
        for (u32 i = 0; i < BODY; i++) {
            T_EQ(b[i], fill_of(lsn));
        }
        ps_snapshot_unpin(snap, ids[lsn]);
    }
    ps_snapshot_close(snap);
}

/* ids[lsn] records the allocated page so we can check it after reboot. */
static void build(ps_page_id *ids)
{
    fv = fault_vfs_create(7);
    ps_store *s = open_store(PS_CREATE);
    ps_wal *w = open_wal(s);
    for (u64 lsn = 1; lsn <= NREC; lsn++) {
        ids[lsn] = logged_op(s, w, lsn);
        if (lsn == CKPT_LSN) {
            T_OK(ps_checkpoint(s));
        }
    }
    T_EQ(ps_durable_lsn(s).raw, CKPT_LSN);
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    close_both(s, w);
}

/* Return the replay error with both handles open so tests can inspect them. */
static int reopen_recover(ps_apply_ctx *ctx, ps_store **s, ps_wal **w)
{
    *s = open_store(0);
    *w = open_wal(*s);
    return ps_recover(*s, *w, apply_rec, ctx);
}

/* ---- tests ---- */

static void test_recover_applies(void)
{
    ps_page_id ids[NREC + 1];
    build(ids);
    ps_store *s = NULL;
    ps_wal *w = NULL;
    ps_apply_ctx ctx;
    ctx_init(&ctx, APPLY_OK);
    T_OK(reopen_recover(&ctx, &s, &w));
    T_EQ(ctx.nseen, NREC - CKPT_LSN);
    for (u32 i = 0; i < ctx.nseen; i++) {
        T_EQ(ctx.seen[i], CKPT_LSN + 1 + i);
    }
    T_EQ(ps_committed_lsn(s).raw, NREC);
    T_EQ(ps_durable_lsn(s).raw, CKPT_LSN);
    check_pages(s, ids, NREC);

    const ps_page_id id = logged_op(s, w, NREC + 1);
    T_OK(ps_checkpoint(s));
    T_EQ(ps_durable_lsn(s).raw, NREC + 1);
    close_both(s, w);

    ctx_init(&ctx, APPLY_OK);
    T_OK(reopen_recover(&ctx, &s, &w));
    T_EQ(ctx.nseen, 0);
    T_EQ(ps_committed_lsn(s).raw, NREC + 1);
    ps_page_id all[NREC + 2];
    memcpy(all, ids, sizeof(ids));
    all[NREC + 1] = id;
    check_pages(s, all, NREC + 1);
    T_OK(ps_recover(s, w, apply_rec, &ctx));
    close_both(s, w);
    fault_vfs_destroy(fv);
}

static void test_recover_twice_einval(void)
{
    ps_page_id ids[NREC + 1];
    build(ids);
    ps_store *s = NULL;
    ps_wal *w = NULL;
    ps_apply_ctx ctx;
    ctx_init(&ctx, APPLY_OK);
    T_OK(reopen_recover(&ctx, &s, &w));
    ctx_init(&ctx, APPLY_OK);
    T_ERR(ps_recover(s, w, apply_rec, &ctx), EINVAL);
    T_EQ(ctx.nseen, 0);
    close_both(s, w);
    fault_vfs_destroy(fv);
}

/* A thread that holds an operation open until told to abort it. */
typedef struct op_holder {
    ps_store *s;
    ps_lsn lsn;
    atomic_bool begun;
    atomic_bool release;
} op_holder;

static void *hold_op(void *arg)
{
    op_holder *h = arg;
    T_OK(ps_op_begin(h->s, h->lsn));
    atomic_store(&h->begun, true);
    while (!atomic_load(&h->release)) {
        sched_yield();
    }
    ps_op_abort(h->s);
    return NULL;
}

static void test_recover_misuse(void)
{
    ps_page_id ids[NREC + 1];
    build(ids);
    ps_store *s = open_store(0);
    ps_wal *w = open_wal(s);
    ps_apply_ctx ctx;
    ctx_init(&ctx, APPLY_OK);
    T_ERR(ps_recover(NULL, w, apply_rec, &ctx), EINVAL);
    T_ERR(ps_recover(s, NULL, apply_rec, &ctx), EINVAL);
    T_ERR(ps_recover(s, w, NULL, &ctx), EINVAL);

    ps_config cfg = store_config(true);
    ps_store *other = NULL;
    T_OK(ps_open("/other", &cfg, PS_CREATE, &other));
    T_ERR(ps_recover(other, w, apply_rec, &ctx), EINVAL);
    ps_close(other);

    op_holder h = {.s = s, .lsn = ps_lsn_from(CKPT_LSN + 100)};
    atomic_init(&h.begun, false);
    atomic_init(&h.release, false);
    pthread_t t;
    T_EQ(pthread_create(&t, NULL, hold_op, &h), 0);
    while (!atomic_load(&h.begun)) {
        sched_yield();
    }
    T_ERR(ps_recover(s, w, apply_rec, &ctx), EINVAL);
    atomic_store(&h.release, true);
    T_EQ(pthread_join(t, NULL), 0);
    T_EQ(ctx.nseen, 0);

    T_OK(ps_op_begin(s, ps_lsn_from(CKPT_LSN + 100)));
    T_ERR(ps_recover(s, w, apply_rec, &ctx), EINVAL);
    ps_op_abort(s);
    T_EQ(ctx.nseen, 0);

    T_OK(ps_wal_append(w, ps_lsn_from(NREC + 1), NULL, 0));
    T_ERR(ps_recover(s, w, apply_rec, &ctx), EINVAL);
    T_EQ(ctx.nseen, 0);
    close_both(s, w);
    fault_vfs_destroy(fv);
}

static void test_apply_must_commit(void)
{
    const apply_mode modes[] = {APPLY_NO_OP, APPLY_NO_COMMIT, APPLY_WRONG_LSN};
    for (u32 m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        ps_page_id ids[NREC + 1];
        build(ids);
        ps_store *s = NULL;
        ps_wal *w = NULL;
        ps_apply_ctx ctx;
        ctx_init(&ctx, modes[m]);
        T_ERR(reopen_recover(&ctx, &s, &w), EINVAL);
        T_EQ(ctx.nseen, 1);
        T_EQ(ps_committed_lsn(s).raw, modes[m] == APPLY_WRONG_LSN ? CKPT_LSN + 2 : CKPT_LSN);
        /* Starting another operation checks that recovery released the writer. */
        T_OK(ps_op_begin(s, ps_lsn_from(CKPT_LSN + 1000)));
        ps_op_abort(s);
        close_both(s, w);

        /* Reopen replays the same records because no checkpoint advanced the durable LSN. */
        ctx_init(&ctx, APPLY_OK);
        T_OK(reopen_recover(&ctx, &s, &w));
        T_EQ(ctx.nseen, NREC - CKPT_LSN);
        T_EQ(ps_committed_lsn(s).raw, NREC);
        check_pages(s, ids, NREC);
        close_both(s, w);
        fault_vfs_destroy(fv);
    }
}

static void test_apply_error(void)
{
    const apply_mode modes[] = {APPLY_FAIL, APPLY_FAIL_OPEN};
    for (u32 m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        ps_page_id ids[NREC + 1];
        build(ids);
        ps_store *s = NULL;
        ps_wal *w = NULL;
        ps_apply_ctx ctx;
        ctx_init(&ctx, modes[m]);
        ctx.fail_lsn = CKPT_LSN + 5;
        T_ERR(reopen_recover(&ctx, &s, &w), EPROTO);
        T_EQ(ctx.nseen, 5);
        T_EQ(ps_committed_lsn(s).raw, CKPT_LSN + 4);
        T_OK(ps_op_begin(s, ps_lsn_from(CKPT_LSN + 1000)));
        ps_op_abort(s);
        /* The unplayed record is still on disk and owns this LSN. */
        T_ERR(ps_wal_append(w, ps_lsn_from(CKPT_LSN + 5), NULL, 0), EINVAL);
        close_both(s, w);

        ctx_init(&ctx, APPLY_OK);
        T_OK(reopen_recover(&ctx, &s, &w));
        T_EQ(ctx.nseen, NREC - CKPT_LSN);
        check_pages(s, ids, NREC);
        close_both(s, w);
        fault_vfs_destroy(fv);
    }
}

static void test_crash_during_recovery(void)
{
    /* A checkpoint made during a failed replay survives power loss. */
    ps_page_id ids[NREC + 1];
    build(ids);
    ps_store *s = NULL;
    ps_wal *w = NULL;
    ps_apply_ctx ctx;
    ctx_init(&ctx, APPLY_CKPT_THEN_FAIL);
    ctx.ckpt_lsn = CKPT_LSN + 10;
    ctx.fail_lsn = CKPT_LSN + 15;
    T_ERR(reopen_recover(&ctx, &s, &w), EPROTO);
    T_EQ(ps_durable_lsn(s).raw, CKPT_LSN + 10);
    fault_vfs_reboot(fv, FAULT_RANDOM_UNSYNCED);
    close_both(s, w);

    ctx_init(&ctx, APPLY_OK);
    T_OK(reopen_recover(&ctx, &s, &w));
    T_EQ(ctx.nseen, NREC - CKPT_LSN - 10);
    T_EQ(ctx.seen[0], CKPT_LSN + 11);
    T_EQ(ps_committed_lsn(s).raw, NREC);
    check_pages(s, ids, NREC);
    close_both(s, w);
    fault_vfs_destroy(fv);
}

static void test_recover_fresh(void)
{
    fv = fault_vfs_create(3);
    ps_store *s = open_store(PS_CREATE);
    ps_wal *w = open_wal(s);
    ps_apply_ctx ctx;
    ctx_init(&ctx, APPLY_OK);
    T_OK(ps_recover(s, w, apply_rec, &ctx));
    T_EQ(ctx.nseen, 0);
    T_EQ(ps_committed_lsn(s).raw, 0);
    logged_op(s, w, 1);
    close_both(s, w);
    fault_vfs_destroy(fv);
}

/* ---- POSIX startup sequence ---- */

static void sleep_ms(u32 ms)
{
    struct timespec ts = {.tv_sec = 0, .tv_nsec = (long) ms * 1000000L};
    nanosleep(&ts, NULL);
}

static void test_startup_sequence_posix(void)
{
    char *root = t_tmpdir();
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/store", root);
    ps_config cfg = store_config(false);
    cfg.checkpoint_thread = true;
    cfg.checkpoint_interval_ms = 0;
    const ps_wal_config wc = wal_config();
    ps_page_id ids[NREC + 1];

    ps_store *s = NULL;
    ps_wal *w = NULL;
    ps_apply_ctx ctx;
    T_OK(ps_open(dir, &cfg, PS_CREATE, &s));
    T_OK(ps_wal_open(s, NULL, &wc, &w));
    ctx_init(&ctx, APPLY_OK);
    T_OK(ps_recover(s, w, apply_rec, &ctx));
    T_EQ(ctx.nseen, 0);
    for (u64 lsn = 1; lsn <= 30; lsn++) {
        ids[lsn] = logged_op(s, w, lsn);
    }
    ps_checkpoint_request(s);
    for (u32 i = 0; i < 2000 && ps_durable_lsn(s).raw < 30; i++) {
        sleep_ms(5);
    }
    T_EQ(ps_durable_lsn(s).raw, 30);
    for (u64 lsn = 31; lsn <= NREC; lsn++) {
        ids[lsn] = logged_op(s, w, lsn);
    }
    const u64 d = ps_durable_lsn(s).raw;
    ps_close(s);
    ps_wal_close(w);

    T_OK(ps_open(dir, &cfg, 0, &s));
    T_EQ(ps_durable_lsn(s).raw, d);
    T_OK(ps_wal_open(s, NULL, &wc, &w));
    ctx_init(&ctx, APPLY_OK);
    T_OK(ps_recover(s, w, apply_rec, &ctx));
    T_EQ(ctx.nseen, NREC - d);
    T_EQ(ps_committed_lsn(s).raw, NREC);
    check_pages(s, ids, NREC);
    logged_op(s, w, NREC + 1);
    ps_close(s);
    ps_wal_close(w);
    t_rmtree(root);
    free(root);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_recover_applies);
    T_RUN(test_recover_twice_einval);
    T_RUN(test_recover_misuse);
    T_RUN(test_apply_must_commit);
    T_RUN(test_apply_error);
    T_RUN(test_crash_during_recovery);
    T_RUN(test_recover_fresh);
    T_RUN(test_startup_sequence_posix);
    return t_done();
}
