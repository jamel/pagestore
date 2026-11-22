/*
 * Keep the checkpointer enabled so the writer can wait for frames.
 * Readers can verify a page from its header alone, even while the writer
 * is updating the expected latest LSN for that ID.
 */

#include "allocator.h"
#include "mem_backend.h"
#include "pagestore/pagestore.h"
#include "rng.h"
#include "test.h"
#include "workload.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PS 4096u
#define HDR PS_PAGE_HEADER_SIZE
#define CALLER (PS - HDR)
#define MAGIC 0x53455250u /* "PRES" */

/* Reserve 8 frames for snapshot pins and one for the bitmap, leaving 23. */
#define FRAMES 32u
#define LIMIT 23u
#define MAX_OP_MS 1000u
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static mem_backend *mb;
static ps_store *s;
static u64 next_lsn;
static u32 pattern_len;

static ps_config pressure_config(u32 frames)
{
    ps_config cfg = mb_config(frames);
    cfg.checkpoint_thread = true;
    return cfg;
}

static void open_cfg(const ps_config *cfg, u32 flags)
{
    ps_backend be;
    mb_backend(mb, &be);
    T_OK(ps_open_backend(&be, cfg, flags, &s));
}

static void open_store(const ps_config *cfg)
{
    mb = mb_create();
    open_cfg(cfg, PS_CREATE);
    next_lsn = 1;
    pattern_len = CALLER;
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

static ps_pressure pressure(void)
{
    ps_pressure p;
    ps_op_pressure(s, &p);
    return p;
}

/* ---- page contents ---- */

static u64 fill_of(u32 id, u64 lsn)
{
    u64 z = ((u64) id << 32) ^ (lsn * 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Writes the version (id, lsn): magic and the caller-area pattern. */
static void stamp(ps_page *p, u32 id, u64 lsn)
{
    p->magic = MAGIC;
    w_pattern_fill(fill_of(id, lsn), 0, (u8 *) p + HDR, pattern_len);
}

/* Validate the payload against its header LSN. The caller checks visibility. */
static bool self_valid(const ps_page *p, u32 id)
{
    if (p->magic != MAGIC || p->page_id != id || p->checksum != 0 || p->flags != 0) {
        return false;
    }
    u8 want[CALLER];
    w_pattern_fill(fill_of(id, p->lsn), 0, want, pattern_len);
    return memcmp((const u8 *) p + HDR, want, pattern_len) == 0;
}

/* True if p is exactly the version of id written at lsn. */
static bool is_version(const ps_page *p, u32 id, u64 lsn)
{
    return p->lsn == lsn && self_valid(p, id);
}

static void begin(void)
{
    T_OK(ps_op_begin(s, next_lsn++));
}

/* The LSN from the last begin, even after its operation has committed. */
static u64 op_lsn(void)
{
    return next_lsn - 1;
}

static u32 alloc_page(void)
{
    u32 id = 0;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    stamp(p, id, op_lsn());
    return id;
}

static void write_page(u32 id)
{
    ps_page *p = NULL;
    T_OK(ps_write(s, id, &p));
    stamp(p, id, op_lsn());
}

/* Start with an empty store and checkpoint the initial pages before the stress run. */
static void make_pages(u32 n, u32 per, u64 *ver)
{
    u32 done = 0;
    while (done < n) {
        begin();
        for (u32 i = 0; i < per && done < n; i++) {
            T_EQ(alloc_page(), done + 1);
            done++;
            ver[done] = op_lsn();
        }
        ps_op_commit(s);
    }
    T_OK(ps_checkpoint(s));
}

/* Call after readers close their snapshots. ver uses LSN 0 for an unallocated ID. */
static void check_all(const u64 *ver, u32 n)
{
    T_OK(ps_checkpoint(s));
    const struct ps_stats st = stats();
    T_CHECK(!st.failed);
    T_EQ(st.frames_pinned, 0);
    T_EQ(st.frames_retired, 0);
    T_EQ(st.frames_dirty, 0);
    T_EQ(st.snapshots_open, 0);
    T_EQ(st.frames_free + st.frames_evictable + st.num_groups, st.frames_total);
    u64 live = 0;
    ps_snapshot *sn = NULL;
    T_OK(ps_snapshot_open(s, &sn));
    for (u32 id = 1; id <= n; id++) {
        if (ver[id] == 0) {
            continue;
        }
        const ps_page *p = NULL;
        T_OK(ps_snapshot_read(sn, id, &p));
        T_CHECK(is_version(p, id, ver[id]));
        ps_snapshot_unpin(sn, id);
        live++;
    }
    ps_snapshot_close(sn);
    T_EQ(st.allocated_pages, live);
}

/* ps_close, then reopen on the same backend with cfg. */
static void reopen(const ps_config *cfg)
{
    ps_close(s);
    open_cfg(cfg, 0);
    T_EQ(ps_committed_lsn(s), next_lsn - 1);
}

/* Pass the frame cost of the next action before attempting it. */
static bool chunk(u32 need)
{
    const ps_pressure p = pressure();
    if (p.op_frames + need <= p.op_limit) {
        return false;
    }
    ps_op_commit(s);
    begin();
    return true;
}

/* Returns elapsed milliseconds so the caller can check for writer stalls. */
static u64 rewrite_op(rng *g, u64 *ver, u32 npages)
{
    const u64 t0 = t_now_ms();
    begin();
    const u32 a = 1 + (u32) rng_below(g, npages);
    const u32 b = 1 + (u32) rng_below(g, npages);
    write_page(a);
    if (b != a) {
        write_page(b);
    }
    ps_op_commit(s);
    ver[a] = op_lsn();
    ver[b] = op_lsn();
    return t_now_ms() - t0;
}

/* ---- snapshot cut-offs ---- */

enum {
    LS_PAGES = 48,
    LS_PINNED = 4,
    LS_OPS = 2000
};

static void test_long_snapshot_writer_never_stalls(void)
{
    const ps_config cfg = pressure_config(FRAMES);
    open_store(&cfg);
    u64 ver[LS_PAGES + 1] = {0};
    make_pages(LS_PAGES, 10, ver);
    u64 at_s[LS_PAGES + 1];
    memcpy(at_s, ver, sizeof(at_s));

    ps_snapshot *sn = NULL;
    T_OK(ps_snapshot_open(s, &sn));
    const ps_page *held[LS_PINNED];
    u8 *copy = malloc((usize) LS_PINNED * PS);
    T_CHECK(copy != NULL);
    for (u32 i = 0; i < LS_PINNED; i++) {
        T_OK(ps_snapshot_read(sn, i + 1, &held[i]));
        T_CHECK(is_version(held[i], i + 1, at_s[i + 1]));
        memcpy(copy + (usize) i * PS, held[i], PS);
    }

    rng g;
    rng_seed(&g, 0x10A6u);
    u64 max_ms = 0;
    u64 stale_at = 0;
    u64 sreads = 0;
    const u64 ops = (u64) LS_OPS * t_scale();
    for (u64 k = 1; k <= ops; k++) {
        const u64 t0 = t_now_ms();
        begin();
        const u32 n = 1 + (u32) rng_below(&g, 3);
        u32 ids[3];
        for (u32 i = 0; i < n; i++) {
            bool dup = true;
            while (dup) {
                ids[i] = 1 + (u32) rng_below(&g, LS_PAGES);
                dup = false;
                for (u32 j = 0; j < i; j++) {
                    dup = dup || ids[j] == ids[i];
                }
            }
            const ps_page *rp = NULL;
            T_OK(ps_read(s, ids[i], &rp));
            T_CHECK(is_version(rp, ids[i], ver[ids[i]]));
            write_page(ids[i]);
        }
        ps_op_commit(s);
        for (u32 i = 0; i < n; i++) {
            ver[ids[i]] = op_lsn();
        }
        const u64 el = t_now_ms() - t0;
        max_ms = el > max_ms ? el : max_ms;

        if (stale_at == 0 || k % 25 == 0) {
            /* Read an unpinned ID to exercise lookup at the snapshot LSN. */
            const u32 id = LS_PINNED + 1 + (u32) rng_below(&g, LS_PAGES - LS_PINNED);
            const ps_page *p = NULL;
            const int rv = ps_snapshot_read(sn, id, &p);
            if (stale_at != 0) {
                T_ERR(rv, ESTALE);
            } else if (rv == ESTALE) {
                stale_at = k;
            } else if (rv != ENOBUFS) {
                T_OK(rv);
                T_CHECK(is_version(p, id, at_s[id]));
                ps_snapshot_unpin(sn, id);
                sreads++;
            }
        }
    }
    T_CHECK(max_ms < MAX_OP_MS);
    struct ps_stats st = stats();
    T_CHECK(!st.failed);
    T_EQ(st.snapshots_cut_off, 1);
    T_CHECK(st.checkpoints_run > 1);
    const ps_page *p = NULL;
    T_ERR(ps_snapshot_read(sn, LS_PAGES, &p), ESTALE);
    T_ERR(ps_snapshot_read(sn, 1, &p), ESTALE); /* even a pinned id */
    for (u32 i = 0; i < LS_PINNED; i++) {
        T_MEMEQ(held[i], copy + (usize) i * PS, PS);
    }
    fprintf(
        stderr,
        "  long snapshot: %llu ops, max %llu ms, cut off after op %llu, %llu reads before\n",
        (unsigned long long) ops,
        (unsigned long long) max_ms,
        (unsigned long long) stale_at,
        (unsigned long long) sreads);
    T_CHECK(stale_at != 0);
    ps_snapshot_unpin(sn, 1);
    ps_snapshot_close(sn);
    free(copy);

    check_all(ver, LS_PAGES);
    reopen(&cfg);
    check_all(ver, LS_PAGES);
    close_store();
}

/*
 * Keep snapshots in opening order. If cut-off takes the oldest first,
 * every ESTALE precedes the first live snapshot in a probe.
 */
enum {
    CO_SNAPS = 5,
    CO_OPS = 3000
};

typedef struct co_snap {
    ps_snapshot *sn;
    u64 at[LS_PAGES + 1]; /* ver at its LSN */
    u32 pinned;
    const ps_page *held;
    u8 copy[PS];
    bool dead;
} co_snap;

static void co_open(co_snap *c, const u64 *ver, rng *g)
{
    T_OK(ps_snapshot_open(s, &c->sn));
    memcpy(c->at, ver, sizeof(c->at));
    c->pinned = 1 + (u32) rng_below(g, LS_PAGES);
    c->dead = false;
    T_OK(ps_snapshot_read(c->sn, c->pinned, &c->held));
    T_CHECK(is_version(c->held, c->pinned, c->at[c->pinned]));
    memcpy(c->copy, c->held, PS);
}

static void co_close(co_snap *c)
{
    T_MEMEQ(c->held, c->copy, PS);
    ps_snapshot_close(c->sn);
    c->sn = NULL;
}

static void test_cutoff_oldest_first(void)
{
    const ps_config cfg = pressure_config(FRAMES);
    open_store(&cfg);
    u64 ver[LS_PAGES + 1] = {0};
    make_pages(LS_PAGES, 10, ver);
    co_snap *c = calloc(CO_SNAPS, sizeof(*c)); /* oldest first */
    T_CHECK(c != NULL);
    rng g;
    rng_seed(&g, 0xC0FFu);
    u32 nopen = 0;
    u64 dead_seen = 0;
    u64 max_ms = 0;
    const u64 ops = (u64) CO_OPS * t_scale();
    for (u64 k = 1; k <= ops; k++) {
        if (nopen < CO_SNAPS && rng_below(&g, 8) == 0) {
            co_open(&c[nopen++], ver, &g);
        }
        max_ms = MAX(max_ms, rewrite_op(&g, ver, LS_PAGES));

        bool live_seen = false;
        for (u32 i = 0; i < nopen; i++) {
            u32 id = 1 + (u32) rng_below(&g, LS_PAGES);
            if (id == c[i].pinned) {
                id = id % LS_PAGES + 1;
            }
            const ps_page *p = NULL;
            const int rv = ps_snapshot_read(c[i].sn, id, &p);
            if (rv == ESTALE) {
                T_CHECK(!live_seen);
                if (!c[i].dead) {
                    dead_seen++;
                }
                c[i].dead = true;
                continue;
            }
            T_CHECK(!c[i].dead);
            live_seen = true;
            if (rv == ENOBUFS) {
                continue;
            }
            T_OK(rv);
            T_CHECK(is_version(p, id, c[i].at[id]));
            ps_snapshot_unpin(c[i].sn, id);
        }
        /* Closing the oldest live snapshot occasionally makes room for new ones. */
        u32 drop = 0;
        while (drop < nopen && c[drop].dead) {
            drop++;
        }
        if (drop == 0 && nopen == CO_SNAPS && rng_below(&g, 4) == 0) {
            drop = 1;
        }
        for (u32 i = 0; i < drop; i++) {
            co_close(&c[i]);
        }
        memmove(c, c + drop, (nopen - drop) * sizeof(*c));
        nopen -= drop;
    }
    for (u32 i = 0; i < nopen; i++) {
        co_close(&c[i]);
    }
    free(c);
    const struct ps_stats st = stats();
    fprintf(
        stderr,
        "  oldest first: %llu ops (max %llu ms), %llu cut off\n",
        (unsigned long long) ops,
        (unsigned long long) max_ms,
        (unsigned long long) st.snapshots_cut_off);
    T_CHECK(max_ms < MAX_OP_MS);
    T_CHECK(dead_seen > 1);
    T_EQ(st.snapshots_cut_off, dead_seen);
    check_all(ver, LS_PAGES);
    close_store();
}

/* ---- pin limits ---- */

enum {
    PL_PAGES = 16,
    PL_CAP = 4,
    PL_OPS = 500,
    PL_READERS = 4,
    PL_LIVE = 40,
    PL_CONC_OPS = 1500
};

typedef struct pl_reader {
    pthread_t t;
    u64 seed;
    u64 reads;
    u64 enobufs;
    u64 stale;
} pl_reader;

static atomic_bool pl_stop;

/* Opens snapshots and pins pages until a limit refuses, checking each page. */
static void *pl_reader_main(void *arg)
{
    pl_reader *r = arg;
    rng g;
    rng_seed(&g, r->seed);
    while (!atomic_load(&pl_stop)) {
        ps_snapshot *sn = NULL;
        T_OK(ps_snapshot_open(s, &sn));
        const u64 S = ps_snapshot_lsn(sn);
        for (u32 k = 0; k < 2 * PL_CAP; k++) {
            const u32 id = 1 + (u32) rng_below(&g, PL_LIVE);
            const ps_page *p = NULL;
            const int rv = ps_snapshot_read(sn, id, &p);
            if (rv == ENOBUFS) {
                r->enobufs++;
                break;
            }
            if (rv == ESTALE) {
                r->stale++;
                break;
            }
            T_OK(rv);
            T_CHECK(p->lsn <= S);
            T_CHECK(self_valid(p, id));
            r->reads++;
        }
        ps_snapshot_close(sn);
    }
    return NULL;
}

static void test_pin_limits(void)
{
    ps_config cfg = pressure_config(FRAMES);
    cfg.snapshot_pin_cap = PL_CAP;
    open_store(&cfg);
    u64 ver[PL_LIVE + 1] = {0};
    make_pages(PL_PAGES, 8, ver);

    ps_snapshot *s1 = NULL;
    ps_snapshot *s2 = NULL;
    ps_snapshot *s3 = NULL;
    T_OK(ps_snapshot_open(s, &s1));
    const ps_page *h1[PL_CAP];
    for (u32 i = 0; i < PL_CAP; i++) {
        T_OK(ps_snapshot_read(s1, i + 1, &h1[i]));
    }
    const ps_page *p = NULL;
    T_ERR(ps_snapshot_read(s1, PL_CAP + 1, &p), ENOBUFS);
    T_OK(ps_snapshot_read(s1, 1, &p));
    T_CHECK(p == h1[0]);
    T_EQ(stats().frames_pinned, PL_CAP);

    /* Give the first two snapshots disjoint pins to fill the global budget. */
    T_OK(ps_snapshot_open(s, &s2));
    const ps_page *h2[PL_CAP];
    for (u32 i = 0; i < PL_CAP; i++) {
        T_OK(ps_snapshot_read(s2, PL_CAP + 1 + i, &h2[i]));
    }
    T_OK(ps_snapshot_open(s, &s3));
    T_ERR(ps_snapshot_read(s3, 2 * PL_CAP + 1, &p), ENOBUFS);
    T_ERR(ps_snapshot_read(s3, 1, &p), ENOBUFS); /* Sharing a page still needs a pin. */
    T_EQ(stats().frames_pinned, 2 * PL_CAP);
    ps_snapshot_unpin(s1, 1);
    T_OK(ps_snapshot_read(s3, 2 * PL_CAP + 1, &p));
    T_CHECK(is_version(p, 2 * PL_CAP + 1, ver[2 * PL_CAP + 1]));
    T_ERR(ps_snapshot_read(s1, 1, &p), ENOBUFS);

    u8 *copy = malloc((usize) 2 * PL_CAP * PS);
    T_CHECK(copy != NULL);
    for (u32 i = 1; i < PL_CAP; i++) {
        memcpy(copy + (usize) i * PS, h1[i], PS);
    }
    for (u32 i = 0; i < PL_CAP; i++) {
        memcpy(copy + (usize) (PL_CAP + i) * PS, h2[i], PS);
    }

    /* With the whole pin budget held, the writer still progresses. */
    rng g;
    rng_seed(&g, 0x9105u);
    u64 max_ms = 0;
    const u64 ops = (u64) PL_OPS * t_scale();
    for (u64 k = 0; k < ops; k++) {
        const u64 el = rewrite_op(&g, ver, PL_PAGES);
        max_ms = el > max_ms ? el : max_ms;
    }
    T_CHECK(max_ms < MAX_OP_MS);
    for (u32 i = 1; i < PL_CAP; i++) {
        T_MEMEQ(h1[i], copy + (usize) i * PS, PS);
    }
    for (u32 i = 0; i < PL_CAP; i++) {
        T_MEMEQ(h2[i], copy + (usize) (PL_CAP + i) * PS, PS);
    }
    free(copy);
    ps_snapshot_close(s1);
    ps_snapshot_close(s2);
    ps_snapshot_close(s3);
    check_all(ver, PL_PAGES);

    begin();
    for (u32 id = PL_PAGES + 1; id <= PL_LIVE; id++) {
        chunk(1);
        T_EQ(alloc_page(), id);
        ver[id] = op_lsn();
    }
    ps_op_commit(s);
    atomic_store(&pl_stop, false);
    pl_reader rd[PL_READERS];
    for (u32 i = 0; i < PL_READERS; i++) {
        memset(&rd[i], 0, sizeof(rd[i]));
        rd[i].seed = 77 + i;
        T_OK(pthread_create(&rd[i].t, NULL, pl_reader_main, &rd[i]));
    }
    max_ms = 0;
    const u64 cops = (u64) PL_CONC_OPS * t_scale();
    for (u64 k = 0; k < cops; k++) {
        const u64 el = rewrite_op(&g, ver, PL_LIVE);
        max_ms = el > max_ms ? el : max_ms;
    }
    atomic_store(&pl_stop, true);
    pl_reader tot;
    memset(&tot, 0, sizeof(tot));
    for (u32 i = 0; i < PL_READERS; i++) {
        T_OK(pthread_join(rd[i].t, NULL));
        tot.reads += rd[i].reads;
        tot.enobufs += rd[i].enobufs;
        tot.stale += rd[i].stale;
    }
    fprintf(
        stderr,
        "  pin limits: %llu writer ops (max %llu ms), %llu reads, %llu ENOBUFS, %llu ESTALE\n",
        (unsigned long long) cops,
        (unsigned long long) max_ms,
        (unsigned long long) tot.reads,
        (unsigned long long) tot.enobufs,
        (unsigned long long) tot.stale);
    T_CHECK(max_ms < MAX_OP_MS);
    T_CHECK(tot.reads > 0);
    T_CHECK(tot.enobufs > 0);
    check_all(ver, PL_LIVE);
    close_store();
}

/* ---- operation frame limit ---- */

enum {
    BIG_DIRTY = 20
};

static void test_op_bigger_than_cache(void)
{
    /* Keep dirty frames around until a frame request wakes the checkpointer. */
    ps_config cfg = pressure_config(FRAMES);
    cfg.checkpoint_interval_ms = 0;
    cfg.checkpoint_dirty_pct = 100;
    open_store(&cfg);
    u64 ver[BIG_DIRTY + LIMIT + 1] = {0};
    begin();
    for (u32 id = 1; id <= BIG_DIRTY; id++) {
        T_EQ(alloc_page(), id);
        ver[id] = op_lsn();
    }
    ps_op_commit(s);
    struct ps_stats st0 = stats();
    T_EQ(st0.frames_dirty, BIG_DIRTY + 1); /* and the bitmap page */
    T_EQ(pressure().op_limit, LIMIT);

    /* Allocations: one frame each, plus group 0's spare. */
    begin();
    u32 n = 0;
    int rv = 0;
    for (;;) {
        u32 id = 0;
        ps_page *p = NULL;
        rv = ps_alloc(s, &id, &p);
        if (rv != 0) {
            break;
        }
        T_EQ(id, BIG_DIRTY + 1 + n);
        stamp(p, id, op_lsn());
        n++;
        T_CHECK(n <= LIMIT);
    }
    T_ERR(rv, ENOBUFS);
    T_EQ(n, LIMIT - 1);
    T_EQ(pressure().op_frames, LIMIT);
    struct ps_stats st = stats();
    T_CHECK(st.writer_waits > st0.writer_waits);
    T_EQ(st.snapshots_cut_off, 0);
    ps_op_abort(s);
    T_EQ(pressure().op_frames, 0);
    st = stats();
    T_EQ(st.allocated_pages, BIG_DIRTY);
    T_EQ(st.frames_pinned, 0);

    /* Copies of committed pages: two frames each (the copy and the version under it). */
    begin();
    u32 written = 0;
    for (u32 id = 1; id <= BIG_DIRTY; id++) {
        ps_page *p = NULL;
        rv = ps_write(s, id, &p);
        if (rv != 0) {
            break;
        }
        memset((u8 *) p + HDR, 0xEE, CALLER);
        written++;
    }
    T_ERR(rv, ENOBUFS);
    T_EQ(written, LIMIT / 2);
    ps_op_abort(s);

    begin();
    for (u32 i = 0; i < 3; i++) {
        T_EQ(alloc_page(), BIG_DIRTY + 1 + i);
        ver[BIG_DIRTY + 1 + i] = op_lsn();
    }
    write_page(1);
    ver[1] = op_lsn();
    ps_op_commit(s);
    check_all(ver, BIG_DIRTY + 3);
    close_store();
}

/* ---- chunked jobs ---- */

enum {
    CJ_PAGES = 5000
};

static void test_chunked_job(void)
{
    const ps_config cfg = pressure_config(FRAMES);
    open_store(&cfg);
    u64 *ver = calloc(CJ_PAGES + 1, sizeof(*ver));
    T_CHECK(ver != NULL);

    /* Once the operation has the group spare, each additional allocation costs one frame. */
    u32 chunks = 0;
    begin();
    for (u32 i = 1; i <= CJ_PAGES; i++) {
        chunks += chunk(1) ? 1 : 0;
        T_EQ(alloc_page(), i);
        ver[i] = op_lsn();
    }
    ps_op_commit(s);
    T_CHECK(chunks >= CJ_PAGES / LIMIT);
    T_CHECK(stats().checkpoints_run > 1);
    check_all(ver, CJ_PAGES);

    /* Budget two frames per action to cover a copy or a free with a bitmap spare. */
    const u32 rounds = t_scale();
    for (u32 r = 0; r < rounds; r++) {
        chunks = 0;
        begin();
        for (u32 id = 1; id <= CJ_PAGES; id++) {
            if (ver[id] == 0) {
                continue;
            }
            chunks += chunk(2) ? 1 : 0;
            if (r == 0 && id % 3 == 0) {
                T_OK(ps_free(s, id));
                ver[id] = 0;
            } else {
                write_page(id);
                ver[id] = op_lsn();
            }
        }
        ps_op_commit(s);
        T_CHECK(chunks >= CJ_PAGES / LIMIT);
    }
    check_all(ver, CJ_PAGES);
    reopen(&cfg);
    check_all(ver, CJ_PAGES);

    begin();
    T_EQ(alloc_page(), 3);
    ver[3] = op_lsn();
    ps_op_commit(s);
    check_all(ver, CJ_PAGES);
    free(ver);
    close_store();
}

/* Exhaust group 0 before checking whether a new bitmap survives reopen. */
static void test_group_growth_full(void)
{
    const ps_config cfg = pressure_config(1024);
    open_store(&cfg);
    pattern_len = 64; /* Keep the cost of stamping a full group down. */
    const u32 G = ps_group_size(PS);
    u64 *ver = calloc((usize) G + 3, sizeof(*ver));
    T_CHECK(ver != NULL);

    /* Leave room for a user page, a new group bitmap and its spare. */
    begin();
    for (u32 i = 1; i < G; i++) {
        chunk(3);
        T_EQ(alloc_page(), i);
        ver[i] = op_lsn();
    }
    ps_op_commit(s);
    T_EQ(stats().num_groups, 1);
    T_EQ(stats().allocated_pages, G - 1);

    begin();
    T_EQ(alloc_page(), G + 1); /* G is group 1's bitmap page */
    ver[G + 1] = op_lsn();
    ps_op_commit(s);
    T_EQ(stats().num_groups, 2);
    check_all(ver, G + 1);
    T_EQ(mb_info(mb).num_groups, 2);
    const ps_page *g1 = (const ps_page *) mb_page(mb, G);
    T_CHECK(g1 != NULL);
    T_EQ(g1->flags, PS_PAGE_FLAG_BITMAP);
    T_EQ(g1->page_id, G);
    T_EQ(((const u8 *) g1)[HDR], 0x03); /* the bitmap page itself and G + 1 */

    reopen(&cfg);
    const struct ps_stats st = stats();
    T_EQ(st.num_groups, 2);
    T_EQ(st.allocated_pages, G);
    T_EQ(pressure().op_limit, 1024 - 256 - 2);
    begin();
    T_EQ(alloc_page(), G + 2);
    ver[G + 2] = op_lsn();
    ps_op_commit(s);
    check_all(ver, G + 2);
    free(ver);
    close_store();
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_long_snapshot_writer_never_stalls);
    T_RUN(test_cutoff_oldest_first);
    T_RUN(test_pin_limits);
    T_RUN(test_op_bigger_than_cache);
    T_RUN(test_chunked_job);
    T_RUN(test_group_growth_full);
    return t_done();
}
