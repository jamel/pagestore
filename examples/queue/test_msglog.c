/*
 * Tests for the radix array and the message log. Most cases compare them
 * against a model after random changes, some of them aborted, and run the
 * structure checks to catch leaked or misplaced pages. Values and message
 * bytes are derived from their index, so the model only stores lengths.
 */

#include "msglog.h"
#include "radix.h"
#include "rng.h"
#include "test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE 4096u

/* ---- fixture -------------------------------------------------------------- */

typedef struct fixture {
    char *dir;
    ps_store *s;
    u32 frames;
    u64 lsn; /* the last LSN handed to ps_op_begin */
} fixture;

static void log_line(ps_log_ctx *ctx, int level, const char *msg)
{
    PS_UNUSED(ctx);
    if (level <= PS_LOG_WARN) {
        fprintf(stderr, "store: %s\n", msg);
    }
}

static void fx_open(fixture *f)
{
    ps_config cfg;
    ps_config_init(&cfg);
    cfg.page_size = PAGE;
    cfg.cache_size = (usize) f->frames * PAGE;
    cfg.log = log_line;
    T_OK(ps_open(f->dir, &cfg, PS_CREATE, &f->s));
    f->lsn = ps_committed_lsn(f->s);
}

static fixture fx_new(u32 frames)
{
    fixture f = {.dir = t_tmpdir(), .frames = frames};
    fx_open(&f);
    return f;
}

static void fx_begin(fixture *f)
{
    T_OK(ps_op_begin(f->s, ++f->lsn));
}

/* Makes everything durable, then closes and reopens the store. */
static void fx_reopen(fixture *f)
{
    T_OK(ps_checkpoint(f->s));
    ps_close(f->s);
    fx_open(f);
}

static void fx_destroy(fixture *f)
{
    ps_close(f->s);
    t_rmtree(f->dir);
    free(f->dir);
}

static u64 allocated(const fixture *f)
{
    struct ps_stats st;
    ps_stats(f->s, &st);
    return st.allocated_pages;
}

static u64 mix(u64 x)
{
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    return x + 1;
}

/* ---- radix array ---------------------------------------------------------- */

static u64 rx_value(u64 index, u32 width)
{
    const u64 v = mix(index);
    return width == 4 ? (u32) v : v;
}

static rx_stats rx_checked(fixture *f, const rx_array *a, u64 lo, u64 hi)
{
    ps_snapshot *snap;
    T_OK(ps_snapshot_open(f->s, &snap));
    rx_stats st;
    char msg[256] = "";
    const int rv = rx_check(a, snap, lo, hi, &st, msg, sizeof(msg));
    ps_snapshot_close(snap);
    if (rv != 0) {
        t_fail(__FILE__, __LINE__, "rx_check: %s (%d)", msg, rv);
    }
    T_EQ(allocated(f), st.leaves + st.inner);
    return st;
}

static void test_radix_basic(void)
{
    fixture f = fx_new(256);
    rx_array a;
    T_ERR(rx_init(&a, PAGE, 3), EINVAL);
    T_OK(rx_init(&a, PAGE, 8));
    T_EQ(a.leaf_cap, 507);
    T_EQ(a.fanout, 1014);

    u64 v = 0;
    fx_begin(&f);
    T_ERR(rx_get_op(&a, f.s, 0, &v), ENOENT);
    for (u64 i = 0; i < 10; i++) {
        T_OK(rx_set(&a, f.s, i, i * 3));
    }
    T_OK(rx_get_op(&a, f.s, 9, &v));
    T_EQ(v, 27);
    ps_op_commit(f.s);
    T_EQ(a.height, 1);

    ps_snapshot *snap;
    T_OK(ps_snapshot_open(f.s, &snap));
    T_OK(rx_get(&a, snap, 4, &v));
    T_EQ(v, 12);
    T_OK(rx_get(&a, snap, 300, &v)); /* in a leaf but never set */
    T_EQ(v, 0);
    T_ERR(rx_get(&a, snap, 507, &v), ENOENT);
    ps_snapshot_close(snap);
    rx_checked(&f, &a, 0, 10);

    /* Narrow values must fit. */
    rx_array n;
    T_OK(rx_init(&n, PAGE, 4));
    T_EQ(n.leaf_cap, 1014);
    fx_begin(&f);
    T_ERR(rx_set(&n, f.s, 0, (u64) 1 << 32), EINVAL);
    ps_op_abort(f.s);
    fx_destroy(&f);
}

/* Far indexes add levels above the existing root, and trims free them again. */
static void test_radix_sparse(void)
{
    fixture f = fx_new(256);
    rx_array a;
    T_OK(rx_init(&a, PAGE, 8));
    const u64 far = 100000000000ull; /* needs four levels */
    fx_begin(&f);
    T_OK(rx_set(&a, f.s, 5, 55));
    T_OK(rx_set(&a, f.s, far, 77));
    ps_op_commit(f.s);
    T_EQ(a.height, 4);
    T_EQ(allocated(&f), 7); /* one root, a path of three to each leaf */

    ps_snapshot *snap;
    u64 v;
    T_OK(ps_snapshot_open(f.s, &snap));
    T_OK(rx_get(&a, snap, 5, &v));
    T_EQ(v, 55);
    T_OK(rx_get(&a, snap, far, &v));
    T_EQ(v, 77);
    T_ERR(rx_get(&a, snap, far - 600, &v), ENOENT);
    ps_snapshot_close(snap);

    /* Dropping everything below far leaves only its path. */
    fx_begin(&f);
    T_OK(rx_trim(&a, f.s, 0, far));
    ps_op_commit(f.s);
    rx_checked(&f, &a, far, far + 1);
    T_EQ(allocated(&f), 4);

    /* A bound past the whole tree frees it, and the array starts over. */
    fx_begin(&f);
    T_OK(rx_trim(&a, f.s, far, UINT64_MAX));
    ps_op_commit(f.s);
    T_EQ(a.root, 0);
    T_EQ(a.height, 0);
    T_EQ(allocated(&f), 0);
    fx_begin(&f);
    T_OK(rx_set(&a, f.s, 3, 33));
    ps_op_commit(f.s);
    T_EQ(a.height, 1);
    rx_checked(&f, &a, 3, 4);
    fx_destroy(&f);
}

/* Checks values in [lo, hi), and that leaves wholly below lo are gone. */
static void rx_verify(fixture *f, const rx_array *a, u64 lo, u64 hi, rng *r)
{
    ps_snapshot *snap;
    T_OK(ps_snapshot_open(f->s, &snap));
    u64 v;
    for (u32 i = 0; i < 300 && lo < hi; i++) {
        const u64 idx = lo + rng_below(r, hi - lo);
        T_OK(rx_get(a, snap, idx, &v));
        T_EQ(v, rx_value(idx, a->width));
    }
    const u64 freed = lo - lo % a->leaf_cap;
    if (freed > 0) {
        T_ERR(rx_get(a, snap, rng_below(r, freed), &v), ENOENT);
    }
    ps_snapshot_close(snap);
    rx_checked(f, a, lo, hi);
}

/*
 * Appends and trims a sliding window that starts just below the point
 * where the tree needs a third level, so it grows while holding values.
 */
static void radix_model(u32 width, u64 seed)
{
    fixture f = fx_new(1024);
    rng r;
    rng_seed(&r, seed);
    rx_array a;
    T_OK(rx_init(&a, PAGE, width));
    const u64 two_levels = (u64) a.leaf_cap * a.fanout;
    u64 lo = two_levels - 20000;
    u64 hi = lo;
    const u32 nops = 120 * t_scale();
    for (u32 op = 0; op < nops; op++) {
        const rx_array saved = a;
        const u64 saved_lo = lo;
        const u64 saved_hi = hi;
        fx_begin(&f);
        const u64 n = rng_below(&r, 1500);
        for (u64 i = 0; i < n; i++, hi++) {
            T_OK(rx_set(&a, f.s, hi, rx_value(hi, width)));
        }
        if (hi - lo > 30000 || rng_below(&r, 3) == 0) {
            const u64 to = lo + rng_below(&r, hi - lo + 1);
            T_OK(rx_trim(&a, f.s, lo, to));
            lo = to;
        }
        for (u32 i = 0; i < 20 && lo < hi; i++) {
            const u64 idx = lo + rng_below(&r, hi - lo);
            u64 v;
            T_OK(rx_get_op(&a, f.s, idx, &v));
            T_EQ(v, rx_value(idx, width));
        }
        if (rng_below(&r, 10) == 0) {
            ps_op_abort(f.s);
            a = saved;
            lo = saved_lo;
            hi = saved_hi;
        } else {
            ps_op_commit(f.s);
        }
        if (op % 8 == 7) {
            rx_verify(&f, &a, lo, hi, &r);
        }
        if (op % 40 == 39) {
            fx_reopen(&f);
        }
    }
    T_CHECK(hi > two_levels);
    T_EQ(a.height, 3);
    rx_verify(&f, &a, lo, hi, &r);
    fx_destroy(&f);
}

static void test_radix_model(void)
{
    radix_model(8, 1);
    radix_model(4, 2);
}

static void test_radix_check_finds_damage(void)
{
    fixture f = fx_new(256);
    rx_array a;
    T_OK(rx_init(&a, PAGE, 8));
    fx_begin(&f);
    for (u64 i = 0; i < 2000; i++) {
        T_OK(rx_set(&a, f.s, i, i));
    }
    ps_op_commit(f.s);
    T_EQ(a.height, 2);

    /* Claiming a smaller range finds the nodes a trim should have freed. */
    ps_snapshot *snap;
    rx_stats st;
    char msg[256];
    T_OK(ps_snapshot_open(f.s, &snap));
    T_ERR(rx_check(&a, snap, 1500, 2000, &st, msg, sizeof(msg)), EBADMSG);
    T_CHECK(strstr(msg, "outside the live range") != NULL);
    T_ERR(rx_check(&a, snap, 0, 3000, &st, msg, sizeof(msg)), EBADMSG);
    T_CHECK(strstr(msg, "is missing") != NULL);
    ps_snapshot_close(snap);

    /* A child in the wrong slot has the wrong base. */
    fx_begin(&f);
    ps_page *root;
    T_OK(ps_write(f.s, a.root, &root));
    u8 *slots = (u8 *) root + 0x28;
    u8 tmp[4];
    memcpy(tmp, slots, 4);
    memcpy(slots, slots + 4, 4);
    memcpy(slots + 4, tmp, 4);
    ps_op_commit(f.s);
    T_OK(ps_snapshot_open(f.s, &snap));
    T_ERR(rx_check(&a, snap, 0, 2000, &st, msg, sizeof(msg)), EBADMSG);
    T_CHECK(strstr(msg, "not the level 0 node at base 0") != NULL);
    u64 v;
    T_ERR(rx_get(&a, snap, 1, &v), EBADMSG);
    ps_snapshot_close(snap);
    fx_destroy(&f);
}

/* ---- message log ---------------------------------------------------------- */

static u8 msg_byte(u64 seq, u32 i)
{
    return (u8) (seq * 7 + i * 13 + 1);
}

static u32 fill_msg(u8 *buf, u64 seq, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        buf[i] = msg_byte(seq, i);
    }
    return len;
}

static void expect_msg(const u8 *buf, u64 seq, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        if (buf[i] != msg_byte(seq, i)) {
            t_fail(
                __FILE__,
                __LINE__,
                "message %llu: byte %u differs",
                (unsigned long long) seq,
                i);
        }
    }
}

/* Creates a log in its own operation. */
static void log_create(fixture *f, u64 cap, ml_log *l)
{
    u32 meta;
    fx_begin(f);
    T_OK(ml_create(f->s, cap, &meta));
    ps_op_commit(f->s);
    ml_open(f->s, meta, l);
}

static ml_stats log_checked(fixture *f, const ml_log *l)
{
    ps_snapshot *snap;
    T_OK(ps_snapshot_open(f->s, &snap));
    ml_stats st;
    char msg[256] = "";
    const int rv = ml_check(l, snap, &st, msg, sizeof(msg));
    ps_snapshot_close(snap);
    if (rv != 0) {
        t_fail(__FILE__, __LINE__, "ml_check: %s (%d)", msg, rv);
    }
    T_EQ(allocated(f), 1 + st.data_pages + st.index_pages + st.map_pages);
    return st;
}

static void test_log_basic(void)
{
    fixture f = fx_new(256);
    ml_log l;
    fx_begin(&f);
    u32 meta;
    T_ERR(ml_create(f.s, 0, &meta), EINVAL);
    ps_op_abort(f.s);
    log_create(&f, 100, &l);
    T_EQ(l.data_size, PAGE - 32);
    log_checked(&f, &l);

    /* An empty message, a small one and one that spans three pages. */
    const u32 lens[] = {0, 10, 2 * l.data_size + 100};
    u8 *buf = malloc(3 * PAGE);
    T_CHECK(buf != NULL);
    fx_begin(&f);
    for (u64 i = 0; i < 3; i++) {
        u64 seq;
        T_OK(ml_push(&l, buf, fill_msg(buf, i, lens[i]), &seq));
        T_EQ(seq, i);
    }
    ps_op_commit(f.s);
    ml_stats st = log_checked(&f, &l);
    T_EQ(st.messages, 3);
    T_EQ(st.bytes, 10 + 2 * l.data_size + 100);
    T_EQ(st.data_pages, 3);

    ps_snapshot *snap;
    T_OK(ps_snapshot_open(f.s, &snap));
    ml_state s;
    T_OK(ml_state_get(&l, snap, &s));
    T_EQ(s.first_seq, 0);
    T_EQ(s.next_seq, 3);
    T_EQ(s.cap, 100);
    for (u64 i = 0; i < 3; i++) {
        u32 len;
        memset(buf, 0, 3 * PAGE);
        T_OK(ml_read(&l, snap, i, buf, 3 * PAGE, &len));
        T_EQ(len, lens[i]);
        expect_msg(buf, i, len);
    }
    /* A short buffer gets a prefix and the full length. */
    u32 len;
    T_OK(ml_read(&l, snap, 2, buf, 5, &len));
    T_EQ(len, lens[2]);
    expect_msg(buf, 2, 5);
    T_ERR(ml_read(&l, snap, 3, buf, 3 * PAGE, &len), ENOENT);
    ps_snapshot_close(snap);

    /* Readers. */
    fx_begin(&f);
    T_OK(ml_reader_add(&l, "a", 0));
    T_OK(ml_reader_add(&l, "b", UINT64_MAX)); /* clamped to the next new message */
    T_OK(ml_reader_add(&l, "c", 2));
    T_OK(ml_reader_ack(&l, "a", 2));
    T_OK(ml_reader_ack(&l, "a", 1)); /* backwards changes nothing */
    T_OK(ml_reader_remove(&l, "c"));
    ps_op_commit(f.s);
    st = log_checked(&f, &l);
    T_EQ(st.readers, 2);

    T_OK(ps_snapshot_open(f.s, &snap));
    u64 pos;
    T_OK(ml_reader_get(&l, snap, "a", &pos));
    T_EQ(pos, 2);
    T_OK(ml_reader_get(&l, snap, "b", &pos));
    T_EQ(pos, 3);
    T_ERR(ml_reader_get(&l, snap, "c", &pos), ENOENT);
    u32 slot = 0;
    char name[ML_NAME_MAX + 1];
    T_OK(ml_reader_next(&l, snap, &slot, name, &pos));
    T_CHECK(strcmp(name, "a") == 0);
    T_OK(ml_reader_next(&l, snap, &slot, name, &pos));
    T_CHECK(strcmp(name, "b") == 0);
    T_ERR(ml_reader_next(&l, snap, &slot, name, &pos), ENOENT);
    ps_snapshot_close(snap);

    /* Each failing change runs in its own operation, which is then aborted. */
    const struct {
        const char *name;
        u64 pos;
        int rv;
    } bad_adds[] = {
        {"a", 0, EEXIST},
        {"", 0, EINVAL},
        {"a-name-longer-than-23-bb", 0, EINVAL},
    };
    for (usize i = 0; i < sizeof(bad_adds) / sizeof(bad_adds[0]); i++) {
        fx_begin(&f);
        T_ERR(ml_reader_add(&l, bad_adds[i].name, bad_adds[i].pos), bad_adds[i].rv);
        ps_op_abort(f.s);
    }
    fx_begin(&f);
    T_ERR(ml_reader_ack(&l, "a", 4), EINVAL);
    ps_op_abort(f.s);
    fx_begin(&f);
    T_ERR(ml_reader_remove(&l, "zzz"), ENOENT);
    ps_op_abort(f.s);

    /* The table holds max_readers names. */
    fx_begin(&f);
    for (u32 i = 2; i < l.max_readers; i++) {
        char n[16];
        snprintf(n, sizeof(n), "r%u", i);
        T_OK(ml_reader_add(&l, n, 0));
    }
    ps_op_commit(f.s);
    fx_begin(&f);
    T_ERR(ml_reader_add(&l, "one-more", 0), ENOSPC);
    ps_op_abort(f.s);
    st = log_checked(&f, &l);
    T_EQ(st.readers, l.max_readers);

    fx_reopen(&f);
    ml_open(f.s, l.meta_id, &l);
    log_checked(&f, &l);
    free(buf);
    fx_destroy(&f);
}

/* Everything the log should hold. Messages are stored as lengths. */
typedef struct log_model {
    u64 cap;
    u64 first;
    u64 next;
    u32 *lens; /* indexed by sequence number */
    u64 lens_cap;
    u64 pos[8]; /* reader positions, UINT64_MAX when absent */
} log_model;

#define MODEL_READERS 8u

static void model_reader_name(u32 i, char *out)
{
    snprintf(out, 8, "r%u", i);
}

static u32 random_len(rng *r, u32 d)
{
    const u64 k = rng_below(r, 100);
    if (k < 5) {
        return 0;
    }
    if (k < 60) {
        return 1 + (u32) rng_below(r, 100);
    }
    if (k < 85) {
        return 1 + (u32) rng_below(r, d);
    }
    return d + (u32) rng_below(r, 2 * d);
}

static void model_push(log_model *m, u32 len)
{
    if (m->next == m->lens_cap) {
        m->lens_cap = m->lens_cap == 0 ? 1024 : 2 * m->lens_cap;
        m->lens = realloc(m->lens, m->lens_cap * sizeof(*m->lens));
        T_CHECK(m->lens != NULL);
    }
    m->lens[m->next++] = len;
    if (m->next - m->first > m->cap) {
        m->first = m->next - m->cap;
    }
}

static void log_verify(fixture *f, const ml_log *l, const log_model *m, rng *r, u8 *buf, u32 cap)
{
    ps_snapshot *snap;
    T_OK(ps_snapshot_open(f->s, &snap));
    ml_state st;
    T_OK(ml_state_get(l, snap, &st));
    T_EQ(st.first_seq, m->first);
    T_EQ(st.next_seq, m->next);
    u64 bytes = 0;
    for (u64 s = m->first; s < m->next; s++) {
        bytes += m->lens[s];
    }
    T_EQ(st.bytes, bytes);
    const u64 kept = m->next - m->first;
    for (u64 i = 0; i < kept && i < 200; i++) {
        const u64 seq = kept <= 200 ? m->first + i : m->first + rng_below(r, kept);
        u32 len;
        T_OK(ml_read(l, snap, seq, buf, cap, &len));
        T_EQ(len, m->lens[seq]);
        expect_msg(buf, seq, len);
    }
    u32 len;
    if (m->first > 0) {
        T_ERR(ml_read(l, snap, m->first - 1, buf, cap, &len), ENOENT);
    }
    T_ERR(ml_read(l, snap, m->next, buf, cap, &len), ENOENT);
    u32 readers = 0;
    for (u32 i = 0; i < MODEL_READERS; i++) {
        char name[8];
        model_reader_name(i, name);
        u64 pos;
        if (m->pos[i] == UINT64_MAX) {
            T_ERR(ml_reader_get(l, snap, name, &pos), ENOENT);
        } else {
            T_OK(ml_reader_get(l, snap, name, &pos));
            T_EQ(pos, m->pos[i]);
            readers++;
        }
    }
    T_EQ(st.readers, readers);
    ps_snapshot_close(snap);
    const ml_stats ms = log_checked(f, l);
    T_EQ(ms.messages, kept);
    T_EQ(ms.bytes, bytes);
}

/* Changes one reader in a way that can't fail. */
static void random_reader_change(ml_log *l, log_model *m, rng *r)
{
    const u32 i = (u32) rng_below(r, MODEL_READERS);
    char name[8];
    model_reader_name(i, name);
    if (m->pos[i] == UINT64_MAX) {
        const u64 k = rng_below(r, 3);
        const u64 pos = k == 0 ? 0 : k == 1 ? UINT64_MAX : rng_below(r, m->next + 10);
        T_OK(ml_reader_add(l, name, pos));
        m->pos[i] = pos < m->first ? m->first : pos > m->next ? m->next : pos;
    } else if (rng_below(r, 4) == 0) {
        T_OK(ml_reader_remove(l, name));
        m->pos[i] = UINT64_MAX;
    } else {
        const u64 pos = rng_below(r, m->next + 1);
        T_OK(ml_reader_ack(l, name, pos));
        if (pos > m->pos[i]) {
            m->pos[i] = pos;
        }
    }
}

static void log_model_run(u64 seed, u64 cap, u32 nops)
{
    fixture f = fx_new(1024);
    rng r;
    rng_seed(&r, seed);
    ml_log l;
    log_create(&f, cap, &l);
    log_model m = {.cap = cap};
    for (u32 i = 0; i < MODEL_READERS; i++) {
        m.pos[i] = UINT64_MAX;
    }
    const u32 buf_cap = 3 * l.data_size;
    u8 *buf = malloc(buf_cap);
    T_CHECK(buf != NULL);
    for (u32 op = 0; op < nops; op++) {
        const log_model saved = m;
        fx_begin(&f);
        const u64 n = rng_below(&r, 8);
        for (u64 i = 0; i < n; i++) {
            const u32 len = random_len(&r, l.data_size);
            u64 seq;
            T_OK(ml_push(&l, buf, fill_msg(buf, m.next, len), &seq));
            T_EQ(seq, m.next);
            model_push(&m, len);
        }
        if (rng_below(&r, 3) == 0) {
            random_reader_change(&l, &m, &r);
        }
        if (rng_below(&r, 10) == 0) {
            ps_op_abort(f.s);
            /* Keep the grown array. Lengths past next get rewritten before use. */
            u32 *lens = m.lens;
            const u64 lens_cap = m.lens_cap;
            m = saved;
            m.lens = lens;
            m.lens_cap = lens_cap;
        } else {
            ps_op_commit(f.s);
        }
        if (op % 10 == 9) {
            log_verify(&f, &l, &m, &r, buf, buf_cap);
        }
        if (op % 97 == 96) {
            fx_reopen(&f);
            ml_open(f.s, l.meta_id, &l);
        }
    }
    log_verify(&f, &l, &m, &r, buf, buf_cap);
    free(m.lens);
    free(buf);
    fx_destroy(&f);
}

static void test_log_model(void)
{
    const u32 nops = 400 * t_scale();
    log_model_run(11, 1, nops / 4);
    log_model_run(12, 37, nops);
    log_model_run(13, 2000, nops);
}

/* A trim frees every data page when the oldest kept message is empty and starts a page. */
static void test_log_trim_all_pages(void)
{
    fixture f = fx_new(256);
    ml_log l;
    log_create(&f, 1, &l);
    u8 *buf = malloc(l.data_size);
    T_CHECK(buf != NULL);
    u64 seq;
    fx_begin(&f);
    T_OK(ml_push(&l, buf, fill_msg(buf, 0, l.data_size), &seq));
    ps_op_commit(f.s);
    fx_begin(&f);
    T_OK(ml_push(&l, buf, 0, &seq));
    ps_op_commit(f.s);
    ml_stats st = log_checked(&f, &l);
    T_EQ(st.messages, 1);
    T_EQ(st.data_pages, 0);

    fx_begin(&f);
    T_OK(ml_push(&l, buf, fill_msg(buf, 2, 10), &seq));
    ps_op_commit(f.s);
    st = log_checked(&f, &l);
    T_EQ(st.data_pages, 1);
    ps_snapshot *snap;
    T_OK(ps_snapshot_open(f.s, &snap));
    u32 len;
    T_OK(ml_read(&l, snap, 2, buf, l.data_size, &len));
    T_EQ(len, 10);
    expect_msg(buf, 2, len);
    ps_snapshot_close(snap);
    free(buf);
    fx_destroy(&f);
}

/* A snapshot keeps reading messages that later pushes trimmed away. */
static void test_log_snapshot(void)
{
    fixture f = fx_new(512);
    ml_log l;
    log_create(&f, 10, &l);
    u8 buf[PAGE];
    u64 seq;
    fx_begin(&f);
    for (u64 i = 0; i < 10; i++) {
        T_OK(ml_push(&l, buf, fill_msg(buf, i, (u32) (i * 300)), &seq));
    }
    ps_op_commit(f.s);

    ps_snapshot *old;
    T_OK(ps_snapshot_open(f.s, &old));
    fx_begin(&f);
    for (u64 i = 10; i < 30; i++) {
        T_OK(ml_push(&l, buf, fill_msg(buf, i, 50), &seq));
    }
    ps_op_commit(f.s);

    ps_snapshot *now;
    T_OK(ps_snapshot_open(f.s, &now));
    u32 len;
    for (u64 i = 0; i < 10; i++) {
        T_OK(ml_read(&l, old, i, buf, sizeof(buf), &len));
        T_EQ(len, i * 300);
        expect_msg(buf, i, len);
        T_ERR(ml_read(&l, now, i, buf, sizeof(buf), &len), ENOENT);
    }
    T_ERR(ml_read(&l, old, 10, buf, sizeof(buf), &len), ENOENT);
    T_OK(ml_read(&l, now, 29, buf, sizeof(buf), &len));
    expect_msg(buf, 29, len);
    ps_snapshot_close(old);
    ps_snapshot_close(now);
    log_checked(&f, &l);
    fx_destroy(&f);
}

/* Reads a u32 field of a committed page. The tests know the page layouts. */
static u32 page_u32(fixture *f, u32 id, u32 off)
{
    ps_snapshot *snap;
    T_OK(ps_snapshot_open(f->s, &snap));
    const ps_page *p;
    T_OK(ps_snapshot_read(snap, id, &p));
    u32 v;
    memcpy(&v, (const u8 *) p + off, sizeof(v));
    ps_snapshot_close(snap);
    return v;
}

static void expect_damage(fixture *f, const ml_log *l, const char *what)
{
    ps_snapshot *snap;
    T_OK(ps_snapshot_open(f->s, &snap));
    ml_stats st;
    char msg[256] = "";
    T_ERR(ml_check(l, snap, &st, msg, sizeof(msg)), EBADMSG);
    ps_snapshot_close(snap);
    if (strstr(msg, what) == NULL) {
        t_fail(__FILE__, __LINE__, "ml_check said '%s', want '%s'", msg, what);
    }
}

static void test_log_check_finds_damage(void)
{
    fixture f = fx_new(256);
    ml_log l;
    log_create(&f, 100, &l);
    u8 buf[PAGE];
    u64 seq;
    fx_begin(&f);
    for (u64 i = 0; i < 20; i++) {
        T_OK(ml_push(&l, buf, fill_msg(buf, i, 1000), &seq));
    }
    ps_op_commit(f.s);
    log_checked(&f, &l);

    /* With 20 small messages, both radix arrays are a single leaf. */
    T_EQ(page_u32(&f, l.meta_id, 0x4C), 1);
    T_EQ(page_u32(&f, l.meta_id, 0x54), 1);
    const u32 leaf = page_u32(&f, l.meta_id, 0x48);
    const u32 data = page_u32(&f, page_u32(&f, l.meta_id, 0x50), 0x28);

    /* A data page that claims another logical page number. */
    ps_page *p;
    fx_begin(&f);
    T_OK(ps_write(f.s, data, &p));
    ((u8 *) p)[0x18] ^= 0x40;
    ps_op_commit(f.s);
    expect_damage(&f, &l, "not the data page");
    u32 len;
    ps_snapshot *snap;
    T_OK(ps_snapshot_open(f.s, &snap));
    T_ERR(ml_read(&l, snap, 0, buf, sizeof(buf), &len), EBADMSG);
    ps_snapshot_close(snap);
    fx_begin(&f);
    T_OK(ps_write(f.s, data, &p));
    ((u8 *) p)[0x18] ^= 0x40;
    ps_op_commit(f.s);
    log_checked(&f, &l);

    /* Two index entries out of order. */
    fx_begin(&f);
    T_OK(ps_write(f.s, leaf, &p));
    u8 *slots = (u8 *) p + 0x28;
    u8 tmp[8];
    memcpy(tmp, slots + 8 * 3, 8);
    memcpy(slots + 8 * 3, slots + 8 * 4, 8);
    memcpy(slots + 8 * 4, tmp, 8);
    ps_op_commit(f.s);
    expect_damage(&f, &l, "index: message 4");

    /* Put them back, then break the reader count. */
    fx_begin(&f);
    T_OK(ps_write(f.s, leaf, &p));
    slots = (u8 *) p + 0x28;
    memcpy(slots + 8 * 4, slots + 8 * 3, 8);
    memcpy(slots + 8 * 3, tmp, 8);
    T_OK(ps_write(f.s, l.meta_id, &p));
    ((u8 *) p)[0x44] = 3;
    ps_op_commit(f.s);
    expect_damage(&f, &l, "count says 3");
    fx_destroy(&f);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_radix_basic);
    T_RUN(test_radix_sparse);
    T_RUN(test_radix_model);
    T_RUN(test_radix_check_finds_damage);
    T_RUN(test_log_basic);
    T_RUN(test_log_model);
    T_RUN(test_log_trim_all_pages);
    T_RUN(test_log_snapshot);
    T_RUN(test_log_check_finds_damage);
    return t_done();
}
