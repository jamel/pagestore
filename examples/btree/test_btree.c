/*
 * Tests for the B+tree example. Most cases compare the tree against a
 * sorted array after random changes, and run bt_check to verify the
 * structure and catch leaked pages. Keys come from a fixed pool with long
 * shared prefixes, so separators are long and inner nodes split, merge
 * and borrow often.
 */

#include "btree.h"
#include "rng.h"
#include "test.h"

#include "pagestore/bytes.h"
#include "pagestore/pagestore.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- fixture -------------------------------------------------------------- */

typedef struct fixture {
    char *dir;
    ps_store *s;
    bt_tree t;
    ps_page_id meta;
    ps_lsn lsn; /* the last LSN handed to ps_op_begin */
} fixture;

static void log_line(ps_logger_ctx *ctx, ps_log_level level, const char *fmt, va_list ap)
{
    PS_UNUSED(ctx, level);
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    fprintf(stderr, "store: %s\n", msg);
}

static void fx_begin(fixture *f)
{
    f->lsn = ps_lsn_next(f->lsn);
    T_OK(ps_op_begin(f->s, f->lsn));
}

/* Opens the store in f->dir, creating the tree if the store is new. */
static void fx_open(fixture *f, u32 page_size, u32 frames)
{
    ps_config cfg;
    ps_config_init(&cfg);
    cfg.page_size = page_size;
    cfg.cache_size = (usize) frames * page_size;
    cfg.logger = (ps_logger) {.max_level = PS_LOG_LEVEL_WARN, .write = log_line};
    T_OK(ps_open(f->dir, &cfg, PS_CREATE, &f->s));
    f->lsn = ps_committed_lsn(f->s);
    if (ps_lsn_is_none(f->lsn)) {
        fx_begin(f);
        T_OK(bt_create(f->s, &f->meta));
        ps_op_commit(f->s);
    }
    T_OK(bt_open(f->s, f->meta, &f->t));
}

static void fx_close(fixture *f)
{
    bt_close(&f->t);
    ps_close(f->s);
    f->s = NULL;
}

static fixture fx_new(u32 page_size, u32 frames)
{
    fixture f = {.dir = t_tmpdir()};
    fx_open(&f, page_size, frames);
    return f;
}

static void fx_destroy(fixture *f)
{
    fx_close(f);
    t_rmtree(f->dir);
    free(f->dir);
}

/* ---- reference model ------------------------------------------------------ */

typedef struct entry {
    u8 *key;
    u32 klen;
    u8 *val;
    u32 vlen;
} entry;

/* Entries sorted by key. */
typedef struct model {
    entry *v;
    usize n;
    usize cap;
} model;

static int cmp_keys(const u8 *a, u32 alen, const u8 *b, u32 blen)
{
    const u32 n = alen < blen ? alen : blen;
    const int c = n == 0 ? 0 : memcmp(a, b, n);
    if (c != 0) {
        return c;
    }
    return alen < blen ? -1 : alen > blen ? 1 : 0;
}

static usize model_find(const model *m, const u8 *key, u32 klen, bool *found)
{
    usize lo = 0;
    usize hi = m->n;
    while (lo < hi) {
        const usize mid = lo + (hi - lo) / 2;
        if (cmp_keys(m->v[mid].key, m->v[mid].klen, key, klen) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *found = lo < m->n && cmp_keys(m->v[lo].key, m->v[lo].klen, key, klen) == 0;
    return lo;
}

static u8 *dup_bytes(const u8 *p, u32 n)
{
    u8 *d = t_malloc(n);
    if (n > 0) {
        memcpy(d, p, n);
    }
    return d;
}

static void model_put(model *m, const u8 *key, u32 klen, const u8 *val, u32 vlen)
{
    bool found;
    const usize i = model_find(m, key, klen, &found);
    if (found) {
        free(m->v[i].val);
        m->v[i].val = dup_bytes(val, vlen);
        m->v[i].vlen = vlen;
        return;
    }
    if (m->n == m->cap) {
        m->cap = m->cap == 0 ? 64 : m->cap * 2;
        m->v = t_realloc(m->v, m->cap * sizeof(*m->v));
    }
    memmove(m->v + i + 1, m->v + i, (m->n - i) * sizeof(*m->v));
    m->v[i] = (entry) {dup_bytes(key, klen), klen, dup_bytes(val, vlen), vlen};
    m->n++;
}

static bool model_delete(model *m, const u8 *key, u32 klen)
{
    bool found;
    const usize i = model_find(m, key, klen, &found);
    if (!found) {
        return false;
    }
    free(m->v[i].key);
    free(m->v[i].val);
    memmove(m->v + i, m->v + i + 1, (m->n - i - 1) * sizeof(*m->v));
    m->n--;
    return true;
}

static void model_free(model *m)
{
    for (usize i = 0; i < m->n; i++) {
        free(m->v[i].key);
        free(m->v[i].val);
    }
    free(m->v);
    *m = (model) {0};
}

/* ---- checks --------------------------------------------------------------- */

static bt_stats check_tree(fixture *f, ps_snapshot *snap)
{
    bt_stats st;
    char msg[256];
    const int rv = bt_check(&f->t, snap, &st, msg, sizeof(msg));
    if (rv != 0) {
        T_FAIL("bt_check: %s (error %d)", msg, rv);
    }
    return st;
}

/* Compares the whole tree with the model and checks that no page leaked. */
static bt_stats verify(fixture *f, const model *m)
{
    ps_snapshot *snap;
    T_OK(ps_snapshot_open(f->s, &snap));
    const bt_stats st = check_tree(f, snap);
    T_EQ(st.keys, m->n);
    struct ps_stats ss;
    ps_stats(f->s, &ss);
    T_EQ(ss.allocated_pages, 1 + st.leaf_pages + st.inner_pages);

    bt_cursor c;
    bt_item it;
    T_OK(bt_cursor_open(&c, &f->t, snap, NULL, 0));
    for (usize i = 0; i < m->n; i++) {
        T_OK(bt_cursor_next(&c, &it));
        T_EQ(it.klen, m->v[i].klen);
        T_MEMEQ(it.key, m->v[i].key, it.klen);
        T_EQ(it.vlen, m->v[i].vlen);
        T_MEMEQ(it.val, m->v[i].val, it.vlen);
    }
    T_ERR(bt_cursor_next(&c, &it), ENOENT);
    bt_cursor_close(&c);
    ps_snapshot_close(snap);
    return st;
}

static void expect_value(fixture *f, ps_snapshot *snap, const char *key, const char *want)
{
    u8 buf[64];
    u32 vlen = 0;
    const int rv = bt_get(&f->t, snap, key, (u32) strlen(key), buf, sizeof(buf), &vlen);
    if (want == NULL) {
        T_ERR(rv, ENOENT);
        return;
    }
    T_OK(rv);
    T_EQ(vlen, strlen(want));
    T_MEMEQ(buf, want, vlen);
}

static void expect_now(fixture *f, const char *key, const char *want)
{
    ps_snapshot *snap;
    T_OK(ps_snapshot_open(f->s, &snap));
    expect_value(f, snap, key, want);
    ps_snapshot_close(snap);
}

/* One change in its own operation. Returns the result of the tree call. */
static int put_str(fixture *f, const char *key, const char *val)
{
    fx_begin(f);
    const int rv = bt_put(&f->t, key, (u32) strlen(key), val, (u32) strlen(val));
    if (rv == 0) {
        ps_op_commit(f->s);
    } else {
        ps_op_abort(f->s);
    }
    return rv;
}

static int del_str(fixture *f, const char *key)
{
    fx_begin(f);
    const int rv = bt_delete(&f->t, key, (u32) strlen(key));
    if (rv == 0) {
        ps_op_commit(f->s);
    } else {
        ps_op_abort(f->s);
    }
    return rv;
}

/* ---- random workload ------------------------------------------------------ */

typedef struct key_pool {
    u32 n;
    u8 **keys;
    u32 *lens;
} key_pool;

/*
 * Each key is one of a few shared prefixes, a short random run of four
 * letters and a unique number. Some keys are padded to max_key bytes.
 */
static key_pool pool_make(rng *r, u32 n, u32 max_key)
{
    static const u32 prefix_lens[] = {0, 3, 40, 300};
    u8 prefixes[4][300];
    for (u32 p = 0; p < 4; p++) {
        for (u32 i = 0; i < prefix_lens[p]; i++) {
            prefixes[p][i] = (u8) ('a' + rng_below(r, 26));
        }
    }
    key_pool kp = {n, t_malloc(n * sizeof(u8 *)), t_malloc(n * sizeof(u32))};
    for (u32 i = 0; i < n; i++) {
        u8 *k = t_malloc(max_key);
        const u32 p = (u32) rng_below(r, 4);
        u32 len = prefix_lens[p];
        memcpy(k, prefixes[p], len);
        const u32 run = 1 + (u32) rng_below(r, 8);
        for (u32 j = 0; j < run; j++) {
            k[len++] = (u8) ('a' + rng_below(r, 4));
        }
        len += (u32) snprintf((char *) k + len, max_key - len, "#%u", i);
        if (rng_below(r, 100) == 0) {
            while (len < max_key) {
                k[len++] = (u8) rng_next(r);
            }
        }
        kp.keys[i] = k;
        kp.lens[i] = len;
    }
    return kp;
}

static void pool_free(key_pool *kp)
{
    for (u32 i = 0; i < kp->n; i++) {
        free(kp->keys[i]);
    }
    free(kp->keys);
    free(kp->lens);
}

/* Mostly short values, some medium ones and a few as long as the entry allows. */
static u32 random_value(rng *r, const bt_tree *t, u32 klen, u8 *buf)
{
    const u64 pick = rng_below(r, 100);
    u32 len;
    if (pick < 85) {
        len = (u32) rng_below(r, 65);
    } else if (pick < 98) {
        len = 64 + (u32) rng_below(r, 337);
    } else {
        len = t->max_entry - klen;
    }
    if (len > t->max_entry - klen) {
        len = t->max_entry - klen;
    }
    for (u32 i = 0; i < len; i++) {
        buf[i] = (u8) rng_next(r);
    }
    return len;
}

typedef struct workload {
    fixture *f;
    model *m;
    rng *r;
    const key_pool *kp;
    u8 *val; /* max_entry bytes */
    u32 max_batch; /* changes in one operation; small caches need small batches */
} workload;

/* One random put or delete. Updates the model too unless the operation will be aborted. */
static void random_change(workload *w, u32 put_pct, bool keep)
{
    const u32 i = (u32) rng_below(w->r, w->kp->n);
    const u8 *key = w->kp->keys[i];
    const u32 klen = w->kp->lens[i];
    if (rng_below(w->r, 100) < put_pct) {
        const u32 vlen = random_value(w->r, &w->f->t, klen, w->val);
        T_OK(bt_put(&w->f->t, key, klen, w->val, vlen));
        if (keep) {
            model_put(w->m, key, klen, w->val, vlen);
        }
        return;
    }
    const int rv = bt_delete(&w->f->t, key, klen);
    if (!keep) {
        T_CHECK(rv == 0 || rv == ENOENT);
        return;
    }
    T_ERR(rv, model_delete(w->m, key, klen) ? 0 : ENOENT);
}

/*
 * Runs nops operations. Most hold one change. Some hold a batch, and a few
 * batches are aborted, which must leave no trace in the tree.
 */
static void run_random(workload *w, u32 nops, u32 put_pct, u32 verify_every)
{
    for (u32 op = 1; op <= nops; op++) {
        const u64 kind = rng_below(w->r, 100);
        fx_begin(w->f);
        if (kind < 3) {
            const u32 n = 1 + (u32) rng_below(w->r, w->max_batch);
            for (u32 j = 0; j < n; j++) {
                random_change(w, put_pct, false);
            }
            ps_op_abort(w->f->s);
        } else {
            const u32 n = kind < 8 ? 1 + (u32) rng_below(w->r, w->max_batch) : 1;
            for (u32 j = 0; j < n; j++) {
                random_change(w, put_pct, true);
            }
            ps_op_commit(w->f->s);
        }
        if (op % verify_every == 0) {
            verify(w->f, w->m);
        }
    }
    verify(w->f, w->m);
}

static void delete_all(fixture *f, model *m)
{
    while (m->n > 0) {
        const entry *e = &m->v[m->n / 2];
        fx_begin(f);
        T_OK(bt_delete(&f->t, e->key, e->klen));
        ps_op_commit(f->s);
        T_CHECK(model_delete(m, e->key, e->klen));
        if (m->n % 500 == 0) {
            verify(f, m);
        }
    }
    const bt_stats st = verify(f, m);
    T_EQ(st.height, 1);
    T_EQ(st.leaf_pages, 1);
}

/* ---- tests ---------------------------------------------------------------- */

static void test_basic(void)
{
    fixture f = fx_new(4096, 256);
    T_EQ(f.t.max_key, 1006);
    T_EQ(f.t.max_entry, 1008);
    expect_now(&f, "a", NULL);

    T_OK(put_str(&f, "b", "2"));
    T_OK(put_str(&f, "a", "1"));
    T_OK(put_str(&f, "c", "3"));
    T_OK(put_str(&f, "b", "two, now longer"));
    T_OK(put_str(&f, "", "empty key"));
    T_OK(put_str(&f, "d", ""));
    expect_now(&f, "a", "1");
    expect_now(&f, "b", "two, now longer");
    expect_now(&f, "", "empty key");
    expect_now(&f, "d", "");
    expect_now(&f, "bb", NULL);

    T_OK(del_str(&f, "c"));
    T_ERR(del_str(&f, "c"), ENOENT);
    expect_now(&f, "c", NULL);

    /* The longest key, and the longest value that fits beside it. */
    u8 *big = t_malloc(f.t.max_entry + 1);
    memset(big, 'z', f.t.max_entry + 1);
    const u32 vmax = f.t.max_entry - f.t.max_key;
    fx_begin(&f);
    T_ERR(bt_put(&f.t, big, f.t.max_key + 1, NULL, 0), EINVAL);
    T_ERR(bt_put(&f.t, big, f.t.max_key, big, vmax + 1), EINVAL);
    T_OK(bt_put(&f.t, big, f.t.max_key, big, vmax));
    ps_op_commit(f.s);

    ps_snapshot *snap;
    T_OK(ps_snapshot_open(f.s, &snap));
    /* A short buffer gets a prefix of the value and its full length. */
    u8 two[2];
    u32 vlen = 0;
    T_OK(bt_get(&f.t, snap, "b", 1, two, sizeof(two), &vlen));
    T_EQ(vlen, strlen("two, now longer"));
    T_MEMEQ(two, "tw", 2);

    /* A cursor starts at the first key at or after its start. */
    bt_cursor c;
    bt_item it;
    T_OK(bt_cursor_open(&c, &f.t, snap, "bb", 2));
    T_OK(bt_cursor_next(&c, &it));
    T_EQ(it.klen, 1);
    T_MEMEQ(it.key, "d", 1);
    T_OK(bt_cursor_next(&c, &it));
    T_EQ(it.klen, f.t.max_key);
    T_ERR(bt_cursor_next(&c, &it), ENOENT);
    T_ERR(bt_cursor_next(&c, &it), ENOENT);
    bt_cursor_close(&c);
    bt_cursor_close(&c);
    ps_snapshot_close(snap);

    model m = {0};
    model_put(&m, (const u8 *) "", 0, (const u8 *) "empty key", 9);
    model_put(&m, (const u8 *) "a", 1, (const u8 *) "1", 1);
    model_put(&m, (const u8 *) "b", 1, (const u8 *) "two, now longer", 15);
    model_put(&m, (const u8 *) "d", 1, (const u8 *) "", 0);
    model_put(&m, big, f.t.max_key, big, vmax);
    verify(&f, &m);
    model_free(&m);
    free(big);
    fx_destroy(&f);
}

static void test_random(void)
{
    fixture f = fx_new(4096, 1024);
    rng r;
    rng_seed(&r, 1);
    key_pool kp = pool_make(&r, 3000, f.t.max_key);
    model m = {0};
    u8 *val = t_malloc(f.t.max_entry);
    workload w = {&f, &m, &r, &kp, val, 16};
    const u32 nops = 4000 * t_scale();
    run_random(&w, nops, 75, 250);
    const bt_stats grown = verify(&f, &m);
    T_CHECK(grown.height >= 3);
    run_random(&w, nops, 30, 250);
    run_random(&w, nops / 2, 60, 250);
    delete_all(&f, &m);
    free(val);
    model_free(&m);
    pool_free(&kp);
    fx_destroy(&f);
}

static bool mostly_full(const bt_stats *st)
{
    return st->leaf_bytes * 100 >= (u64) st->leaf_pages * st->node_capacity * 95;
}

/*
 * Keys on both sides make the load land mid-tree, away from the rightmost
 * path. Puts "a" and "z" in the tree and the model.
 */
static void add_bookends(fixture *f, model *m)
{
    T_OK(put_str(f, "a", "first"));
    T_OK(put_str(f, "z", "last"));
    model_put(m, (const u8 *) "a", 1, (const u8 *) "first", 5);
    model_put(m, (const u8 *) "z", 1, (const u8 *) "last", 4);
}

/* Ascending keys fill leaves completely, and deleting them all shrinks the tree back. */
static void test_ascending(void)
{
    fixture f = fx_new(4096, 1024);
    model m = {0};
    add_bookends(&f, &m);
    const u32 n = 20000;
    char key[32];
    char val[32];
    for (u32 i = 0; i < n; i += 100) {
        fx_begin(&f);
        for (u32 j = i; j < i + 100; j++) {
            const u32 klen = (u32) snprintf(key, sizeof(key), "key%08u", j);
            const u32 vlen = (u32) snprintf(val, sizeof(val), "value %u", j);
            T_OK(bt_put(&f.t, key, klen, val, vlen));
            model_put(&m, (const u8 *) key, klen, (const u8 *) val, vlen);
        }
        ps_op_commit(f.s);
    }
    const bt_stats st = verify(&f, &m);
    T_CHECK(mostly_full(&st));
    for (u32 i = 0; i < n; i++) {
        const u32 klen = (u32) snprintf(key, sizeof(key), "key%08u", i);
        fx_begin(&f);
        T_OK(bt_delete(&f.t, key, klen));
        ps_op_commit(f.s);
        T_CHECK(model_delete(&m, (const u8 *) key, klen));
        if (i % 2000 == 0) {
            verify(&f, &m);
        }
    }
    const bt_stats empty = verify(&f, &m);
    T_EQ(empty.height, 1);
    model_free(&m);
    fx_destroy(&f);
}

static void test_descending(void)
{
    fixture f = fx_new(4096, 1024);
    model m = {0};
    add_bookends(&f, &m);
    char key[32];
    for (u32 i = 10000; i > 0; i--) {
        const u32 klen = (u32) snprintf(key, sizeof(key), "key%08u", i);
        fx_begin(&f);
        T_OK(bt_put(&f.t, key, klen, key, klen));
        ps_op_commit(f.s);
        model_put(&m, (const u8 *) key, klen, (const u8 *) key, klen);
    }
    const bt_stats st = verify(&f, &m);
    T_CHECK(mostly_full(&st));
    delete_all(&f, &m);
    model_free(&m);
    fx_destroy(&f);
}

/*
 * Keys share a 990-byte prefix, so an inner node holds about four
 * separators and merges and borrows happen at every level.
 */
static void test_long_keys(void)
{
    fixture f = fx_new(4096, 1024);
    model m = {0};
    const u32 n = 600;
    u8 key[1006];
    memset(key, 'p', sizeof(key));
    for (u32 i = 0; i < n; i++) {
        const u32 klen = 990 + (u32) snprintf((char *) key + 990, 16, "%06u", i * 7919u % n);
        fx_begin(&f);
        T_OK(bt_put(&f.t, key, klen, "value", 5));
        ps_op_commit(f.s);
        model_put(&m, key, klen, (const u8 *) "value", 5);
    }
    const bt_stats st = verify(&f, &m);
    T_CHECK(st.height >= 4);
    /* Delete from the left end first, then from the middle outward. */
    for (u32 i = 0; i < n / 2; i++) {
        const u32 klen = 990 + (u32) snprintf((char *) key + 990, 16, "%06u", i);
        fx_begin(&f);
        T_OK(bt_delete(&f.t, key, klen));
        ps_op_commit(f.s);
        T_CHECK(model_delete(&m, key, klen));
        if (i % 25 == 0) {
            verify(&f, &m);
        }
    }
    delete_all(&f, &m);
    model_free(&m);
    fx_destroy(&f);
}

/* A snapshot keeps seeing the tree as it was, however much the tree changes later. */
static void test_snapshot(void)
{
    fixture f = fx_new(4096, 1024);
    T_OK(put_str(&f, "k", "old"));
    ps_snapshot *before;
    T_OK(ps_snapshot_open(f.s, &before));

    fx_begin(&f);
    T_OK(bt_put(&f.t, "k", 1, "new", 3));
    T_OK(bt_put(&f.t, "x", 1, "1", 1));
    ps_op_commit(f.s);
    char key[32];
    for (u32 i = 0; i < 3000; i++) {
        snprintf(key, sizeof(key), "fill%u", i * 31u % 3000u);
        T_OK(put_str(&f, key, "some value to make the tree grow"));
    }
    T_OK(del_str(&f, "k"));

    expect_value(&f, before, "k", "old");
    expect_value(&f, before, "x", NULL);
    const bt_stats then = check_tree(&f, before);
    T_EQ(then.keys, 1);
    T_EQ(then.height, 1);
    ps_snapshot_close(before);

    expect_now(&f, "k", NULL);
    expect_now(&f, "x", "1");
    fx_destroy(&f);
}

/* After a checkpoint and reopen, a small cache forces reads and writes through page-in. */
static void test_reopen(void)
{
    fixture f = fx_new(4096, 1024);
    rng r;
    rng_seed(&r, 2);
    key_pool kp = pool_make(&r, 3000, f.t.max_key);
    model m = {0};
    u8 *val = t_malloc(f.t.max_entry);
    workload w = {&f, &m, &r, &kp, val, 16};
    run_random(&w, 3000, 80, 1000);
    T_OK(ps_checkpoint(f.s));
    fx_close(&f);

    fx_open(&f, 4096, 64);
    w.max_batch = 2;
    verify(&f, &m);
    run_random(&w, 2000 * t_scale(), 40, 500);
    struct ps_stats ss;
    ps_stats(f.s, &ss);
    T_CHECK(ss.cache_misses > 0);
    free(val);
    model_free(&m);
    pool_free(&kp);
    fx_destroy(&f);
}

/* Offsets near the end of a 64 KiB page still fit in 16-bit slots. */
static void test_large_pages(void)
{
    fixture f = fx_new(65536, 128);
    T_EQ(f.t.max_entry, (65536u - 40u) / 4u - 6u);
    rng r;
    rng_seed(&r, 3);
    key_pool kp = pool_make(&r, 2000, f.t.max_key);
    model m = {0};
    u8 *val = t_malloc(f.t.max_entry);
    workload w = {&f, &m, &r, &kp, val, 16};
    run_random(&w, 2000, 75, 500);
    run_random(&w, 2000, 30, 500);
    free(val);
    model_free(&m);
    pool_free(&kp);
    fx_destroy(&f);
}

static void swap_first_slots(ps_page *leaf)
{
    u8 *slots = (u8 *) leaf + 0x28; /* NODE_SLOTS in btree.c */
    u8 tmp[2];
    memcpy(tmp, slots, 2);
    memcpy(slots, slots + 2, 2);
    memcpy(slots + 2, tmp, 2);
}

static void expect_damage(fixture *f, const char *what)
{
    ps_snapshot *snap;
    T_OK(ps_snapshot_open(f->s, &snap));
    bt_stats st;
    char msg[256];
    T_ERR(bt_check(&f->t, snap, &st, msg, sizeof(msg)), EBADMSG);
    if (strstr(msg, what) == NULL) {
        T_FAIL("bt_check said '%s', want '%s'", msg, what);
    }
    ps_snapshot_close(snap);
}

/* bt_check reports damage rather than passing over it. */
static void test_check_finds_damage(void)
{
    fixture f = fx_new(4096, 256);
    T_OK(put_str(&f, "a", "1"));
    T_OK(put_str(&f, "b", "2"));
    fx_begin(&f);
    const ps_page *meta;
    T_OK(ps_read(f.s, f.meta, &meta));
    const ps_page_id root = ps_page_id_from(ps_get_le32((const u8 *) meta + PS_PAGE_HEADER_SIZE));
    ps_page *leaf;
    T_OK(ps_write(f.s, root, &leaf));
    swap_first_slots(leaf);
    ps_op_commit(f.s);
    expect_damage(&f, "key 1 is out of order");

    /* Undo the swap, then make the meta page's key count wrong. */
    fx_begin(&f);
    T_OK(ps_write(f.s, root, &leaf));
    swap_first_slots(leaf);
    ps_page *mp;
    T_OK(ps_write(f.s, f.meta, &mp));
    ps_put_le64((u8 *) mp + PS_PAGE_HEADER_SIZE + 8, 3);
    ps_op_commit(f.s);
    expect_damage(&f, "meta page says 3");
    fx_destroy(&f);
}

/*
 * A reader thread checks snapshots while the writer inserts and then deletes
 * keys, one per operation. A snapshot at LSN base + i must hold exactly keys(i).
 */
typedef struct readers_state {
    fixture *f;
    u64 base;
    u64 n;
    atomic_bool stop;
    u64 checks;
} readers_state;

static u64 keys_at(const readers_state *rs, u64 lsn)
{
    const u64 i = lsn - rs->base;
    return i <= rs->n ? i : 2 * rs->n - i;
}

static void *reader_main(void *arg)
{
    readers_state *rs = arg;
    while (!atomic_load(&rs->stop)) {
        ps_snapshot *snap;
        T_OK(ps_snapshot_open(rs->f->s, &snap));
        bt_stats st;
        char msg[256];
        const int rv = bt_check(&rs->f->t, snap, &st, msg, sizeof(msg));
        if (rv != ESTALE) {
            if (rv != 0) {
                T_FAIL("bt_check: %s (error %d)", msg, rv);
            }
            T_EQ(st.keys, keys_at(rs, ps_snapshot_lsn(snap).raw));
            rs->checks++;
        }
        ps_snapshot_close(snap);
    }
    return NULL;
}

static void test_readers(void)
{
    fixture f = fx_new(4096, 2048);
    readers_state rs = {.f = &f, .base = f.lsn.raw, .n = 3000};
    atomic_init(&rs.stop, false);
    pthread_t th;
    T_OK(pthread_create(&th, NULL, reader_main, &rs));
    char key[32];
    for (u64 i = 0; i < 2 * rs.n; i++) {
        const u64 k = (i < rs.n ? i : i - rs.n) * 7919u % rs.n;
        snprintf(key, sizeof(key), "key%llu", (unsigned long long) k);
        if (i < rs.n) {
            T_OK(put_str(&f, key, "a value of moderate length"));
        } else {
            T_OK(del_str(&f, key));
        }
    }
    atomic_store(&rs.stop, true);
    T_OK(pthread_join(th, NULL));
    T_CHECK(rs.checks > 0);
    fx_destroy(&f);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_basic);
    T_RUN(test_random);
    T_RUN(test_ascending);
    T_RUN(test_descending);
    T_RUN(test_long_keys);
    T_RUN(test_snapshot);
    T_RUN(test_reopen);
    T_RUN(test_large_pages);
    T_RUN(test_readers);
    T_RUN(test_check_finds_damage);
    return t_done();
}
