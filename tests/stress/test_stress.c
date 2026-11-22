/*
 * The working set exceeds the cache so readers, the writer and the
 * checkpointer compete for frames. Readers check every version they see,
 * including pages they still hold after a snapshot cut-off.
 */

#include "mem_backend.h"
#include "pagestore/pagestore.h"
#include "rng.h"
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
#define WORDS ((PS - HDR) / 8u)
#define MAGIC 0x53525453u /* "STRS" */

enum {
    FRAMES = 64,
    PIN_CAP = 8,
    READERS = 6,
    LIVE_MAX = 200,
    ID_MAX = 1024, /* Lowest-first allocation stays below this. */
    MAX_ACTIONS = 4,
    MAX_READS = 20,
    RUN_MS = 2000,
    EXTRA_MS = 60000
};

/* ---- page contents ---- */

static u64 mix64(u64 z)
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Regenerate page contents from just the ID and LSN plus the run seed. */
static u64 word_at(u64 seed, u32 id, u64 lsn, u32 i)
{
    if (i == 0) {
        return id;
    }
    if (i == 1) {
        return lsn;
    }
    return mix64(seed ^ mix64(((u64) id << 32) ^ i) ^ (lsn * 0x9E3779B97F4A7C15ull));
}

static void fill_page(u64 seed, ps_page *p, u32 id, u64 lsn)
{
    p->magic = MAGIC;
    u8 *b = (u8 *) p + HDR;
    for (u32 i = 0; i < WORDS; i++) {
        const u64 w = word_at(seed, id, lsn, i);
        memcpy(b + (usize) i * 8, &w, 8);
    }
}

/* The page must be the version of id written at lsn, byte for byte. */
static void check_page(u64 seed, const ps_page *p, u32 id, u64 lsn)
{
    T_EQ(p->page_id, id);
    T_EQ(p->lsn, lsn);
    T_EQ(p->magic, MAGIC);
    T_EQ(p->flags, 0);
    T_EQ(p->checksum, 0);
    const u8 *b = (const u8 *) p + HDR;
    for (u32 i = 0; i < WORDS; i++) {
        u64 w = 0;
        memcpy(&w, b + (usize) i * 8, 8);
        if (w != word_at(seed, id, lsn, i)) {
            t_fail(
                __FILE__,
                __LINE__,
                "page %u (lsn %llu): word %u is %016llx, want %016llx",
                id,
                (unsigned long long) lsn,
                i,
                (unsigned long long) w,
                (unsigned long long) word_at(seed, id, lsn, i));
        }
    }
}

/* ---- history ---- */

/* One committed change of a page: written (allocated or rewritten) or freed at lsn. */
typedef struct hver {
    u64 lsn;
    bool freed;
} hver;

typedef struct hist {
    hver *v; /* ascending lsn */
    u32 n;
    u32 cap;
} hist;

typedef struct stress {
    ps_store *s;
    mem_backend *mb;
    u64 seed;
    atomic_bool stop;
    /* Written before ps_op_commit, so a snapshot at the new LSN finds its versions. */
    pthread_rwlock_t mu;
    hist ids[ID_MAX];
    _Atomic u32 id_end; /* One past the highest ID seen. */
    _Atomic u64 stale; /* ESTALE results seen by the readers */
} stress;

/* Hold mu for reading or writing. A successful lookup gives the page's write LSN. */
static bool hist_at(const stress *st, u32 id, u64 lsn, u64 *lsn_out)
{
    if (id >= ID_MAX) {
        return false;
    }
    const hist *h = &st->ids[id];
    u32 lo = 0;
    u32 hi = h->n;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        if (h->v[mid].lsn <= lsn) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0 || h->v[lo - 1].freed) {
        return false;
    }
    *lsn_out = h->v[lo - 1].lsn;
    return true;
}

static bool hist_lookup(stress *st, u32 id, u64 lsn, u64 *lsn_out)
{
    pthread_rwlock_rdlock(&st->mu);
    const bool live = hist_at(st, id, lsn, lsn_out);
    pthread_rwlock_unlock(&st->mu);
    return live;
}

/* mu held for writing. */
static void hist_push(stress *st, u32 id, u64 lsn, bool freed)
{
    hist *h = &st->ids[id];
    T_CHECK(h->n == 0 || h->v[h->n - 1].lsn < lsn);
    if (h->n == h->cap) {
        h->cap = h->cap == 0 ? 8 : h->cap * 2;
        h->v = realloc(h->v, (usize) h->cap * sizeof(*h->v));
        T_CHECK(h->v != NULL);
    }
    h->v[h->n].lsn = lsn;
    h->v[h->n].freed = freed;
    h->n++;
}

/* ---- helpers ---- */

static void sleep_us(u32 us)
{
    struct timespec ts = {(time_t) (us / 1000000), (long) (us % 1000000) * 1000L};
    while (nanosleep(&ts, &ts) != 0) {
        /* nanosleep puts the remaining delay back in ts on EINTR. */
    }
}

static struct ps_stats get_stats(const ps_store *s)
{
    struct ps_stats out;
    ps_stats(s, &out);
    return out;
}

/* ---- readers ---- */

typedef struct held_page {
    u32 id;
    u64 lsn;
    const ps_page *p;
} held_page;

typedef struct reader {
    pthread_t t;
    stress *st;
    u32 idx;
    u64 seed;
    u64 snapshots;
    u64 reads; /* successful first reads of a page */
    u64 stale; /* snapshots that saw ESTALE */
    u64 busy; /* ENOBUFS from the global budget or the frame request */
    u64 cap_hits; /* ENOBUFS at the snapshot's pin cap */
} reader;

static void check_held(const stress *st, const held_page *h, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        check_page(st->seed, h[i].p, h[i].id, h[i].lsn);
    }
}

static void *reader_main(void *arg)
{
    reader *r = arg;
    stress *st = r->st;
    rng g;
    rng_seed(&g, r->seed);
    u64 last_lsn = 0;
    held_page held[PIN_CAP];
    while (!atomic_load(&st->stop)) {
        ps_snapshot *sn = NULL;
        T_OK(ps_snapshot_open(st->s, &sn));
        r->snapshots++;
        const u64 S = ps_snapshot_lsn(sn);
        T_CHECK(S >= last_lsn);
        last_lsn = S;
        u32 nheld = 0;
        const u32 nreads = 1 + (u32) rng_below(&g, MAX_READS);
        const u32 sleep_at = rng_below(&g, 8) == 0 ? (u32) rng_below(&g, nreads) : UINT32_MAX;
        for (u32 k = 0; k < nreads; k++) {
            if (k == sleep_at) {
                sleep_us(200 + (u32) rng_below(&g, 2000));
            }
            const u32 end = atomic_load(&st->id_end);
            const u32 id = 1 + (u32) rng_below(&g, end > 1 ? end - 1 : 1);
            u64 want = 0;
            if (!hist_lookup(st, id, S, &want)) {
                continue; /* Unallocated IDs are outside this test. */
            }
            u32 have = PIN_CAP;
            for (u32 i = 0; i < nheld; i++) {
                if (held[i].id == id) {
                    have = i;
                }
            }
            const ps_page *p = NULL;
            const int rv = ps_snapshot_read(sn, id, &p);
            if (rv == ESTALE) {
                r->stale++;
                atomic_fetch_add(&st->stale, 1);
                check_held(st, held, nheld);
                T_ERR(ps_snapshot_read(sn, id, &p), ESTALE);
                break;
            }
            if (rv == ENOBUFS) {
                T_CHECK(have == PIN_CAP);
                if (nheld == PIN_CAP) {
                    r->cap_hits++;
                } else {
                    r->busy++;
                }
                break;
            }
            T_OK(rv);
            check_page(st->seed, p, id, want);
            if (have < PIN_CAP) {
                T_CHECK(held[have].p == p);
                continue;
            }
            T_CHECK(nheld < PIN_CAP);
            held[nheld].id = id;
            held[nheld].lsn = want;
            held[nheld].p = p;
            nheld++;
            r->reads++;
            /* Leave some pins held so the reader can still reach its cap. */
            if (nheld >= 4 && rng_below(&g, 3) == 0) {
                const u32 i = (u32) rng_below(&g, nheld);
                check_page(st->seed, held[i].p, held[i].id, held[i].lsn);
                ps_snapshot_unpin(sn, held[i].id);
                held[i] = held[--nheld];
            }
        }
        if (r->idx == 0 && rng_below(&g, 16) == 0) {
            ps_checkpoint_request(st->s);
            const struct ps_stats ss = get_stats(st->s);
            T_CHECK(ss.snapshots_open >= 1);
            T_CHECK(!ss.failed);
        }
        check_held(st, held, nheld);
        ps_snapshot_close(sn);
    }
    return NULL;
}

/* ---- writer ---- */

typedef struct writer {
    stress *st;
    rng g;
    u64 next_lsn;
    u64 last_committed;
    bool live[ID_MAX]; /* committed allocation state */
    u32 live_ids[ID_MAX]; /* the committed live ids, unordered */
    u32 nlive;
    u64 commits;
    u64 aborts;
} writer;

/* All test IDs fit in group 0. The search skips its bitmap ID. */
static u32 next_alloc_id(const writer *w, u32 cursor)
{
    for (u32 id = cursor + 1; id < ID_MAX; id++) {
        if (!w->live[id]) {
            return id;
        }
    }
    t_fail(__FILE__, __LINE__, "ran out of test ids");
}

typedef struct change {
    u32 id;
    bool freed;
} change;

/* Pick from committed IDs untouched by this operation. Returns 0 if no draw fits. */
static u32 pick_live(writer *w, const change *c, u32 nc)
{
    for (u32 tries = 0; tries < 8 && w->nlive > 0; tries++) {
        const u32 id = w->live_ids[rng_below(&w->g, w->nlive)];
        bool touched = false;
        for (u32 i = 0; i < nc; i++) {
            touched = touched || c[i].id == id;
        }
        if (!touched) {
            return id;
        }
    }
    return 0;
}

static void set_live(writer *w, u32 id, bool live)
{
    if (live) {
        T_CHECK(!w->live[id]);
        w->live[id] = true;
        w->live_ids[w->nlive++] = id;
        return;
    }
    T_CHECK(w->live[id]);
    w->live[id] = false;
    for (u32 i = 0; i < w->nlive; i++) {
        if (w->live_ids[i] == id) {
            w->live_ids[i] = w->live_ids[--w->nlive];
            return;
        }
    }
    T_CHECK(!"live id missing");
}

/* One operation of 1..MAX_ACTIONS allocations, frees and rewrites, then commit or abort. */
static void writer_op(writer *w)
{
    stress *st = w->st;
    ps_store *s = st->s;
    const u64 lsn = w->next_lsn++;
    T_OK(ps_op_begin(s, lsn));
    change c[MAX_ACTIONS];
    u32 nc = 0;
    u32 cursor = 0;
    u32 nlive = w->nlive; /* live count in the operation's view */
    const u32 n = 1 + (u32) rng_below(&w->g, MAX_ACTIONS);
    for (u32 k = 0; k < n; k++) {
        const u64 roll = rng_below(&w->g, 10);
        if (nlive < LIVE_MAX && (roll < 4 || w->nlive == 0)) {
            u32 id = 0;
            ps_page *p = NULL;
            T_OK(ps_alloc(s, &id, &p));
            const u32 want = next_alloc_id(w, cursor);
            T_EQ(id, want);
            cursor = id;
            fill_page(st->seed, p, id, lsn);
            c[nc].id = id;
            c[nc].freed = false;
            nc++;
            nlive++;
            continue;
        }
        const u32 id = pick_live(w, c, nc);
        if (id == 0) {
            continue;
        }
        if (roll < 6) {
            T_OK(ps_free(s, id));
            c[nc].id = id;
            c[nc].freed = true;
            nc++;
            nlive--;
            continue;
        }
        u64 cur = 0;
        T_CHECK(hist_lookup(st, id, w->last_committed, &cur));
        const ps_page *old = NULL;
        T_OK(ps_read(s, id, &old));
        check_page(st->seed, old, id, cur);
        ps_page *p = NULL;
        T_OK(ps_write(s, id, &p));
        T_CHECK(p != old);
        T_MEMEQ((const u8 *) p + HDR, (const u8 *) old + HDR, PS - HDR);
        fill_page(st->seed, p, id, lsn);
        check_page(st->seed, old, id, cur);
        c[nc].id = id;
        c[nc].freed = false;
        nc++;
    }
    if (rng_below(&w->g, 16) == 0) {
        ps_op_abort(s);
        w->aborts++;
        return;
    }
    pthread_rwlock_wrlock(&st->mu);
    for (u32 i = 0; i < nc; i++) {
        hist_push(st, c[i].id, lsn, c[i].freed);
        if (c[i].id >= atomic_load(&st->id_end)) {
            atomic_store(&st->id_end, c[i].id + 1);
        }
    }
    pthread_rwlock_unlock(&st->mu);
    ps_op_commit(s);
    T_EQ(ps_committed_lsn(s), lsn);
    w->last_committed = lsn;
    w->commits++;
    for (u32 i = 0; i < nc; i++) {
        if (c[i].freed) {
            set_live(w, c[i].id, false);
        } else if (!w->live[c[i].id]) {
            set_live(w, c[i].id, true);
        }
    }
}

/* Check the same committed history before close and after reopening. */
static void check_final(stress *st, writer *w, ps_store *s)
{
    const u64 L = w->last_committed;
    T_EQ(ps_committed_lsn(s), L);
    ps_snapshot *sn = NULL;
    T_OK(ps_snapshot_open(s, &sn));
    T_EQ(ps_snapshot_lsn(sn), L);
    u32 n = 0;
    for (u32 id = 1; id < atomic_load(&st->id_end); id++) {
        u64 want = 0;
        const bool live = hist_lookup(st, id, L, &want);
        T_EQ(live, w->live[id]);
        if (!live) {
            continue;
        }
        const ps_page *p = NULL;
        T_OK(ps_snapshot_read(sn, id, &p));
        check_page(st->seed, p, id, want);
        ps_snapshot_unpin(sn, id);
        n++;
    }
    ps_snapshot_close(sn);
    T_EQ(n, w->nlive);
    T_EQ(get_stats(s).allocated_pages, w->nlive);
}

static void test_writer_readers_checkpoint(void)
{
    stress *st = calloc(1, sizeof(*st));
    writer *w = calloc(1, sizeof(*w));
    T_CHECK(st != NULL && w != NULL);
    st->seed = 0x5EEDu + t_scale();
    T_EQ(pthread_rwlock_init(&st->mu, NULL), 0);
    atomic_init(&st->stop, false);
    atomic_init(&st->id_end, 1);
    atomic_init(&st->stale, 0);
    st->mb = mb_create();
    ps_config cfg = mb_config(FRAMES);
    cfg.snapshot_pin_cap = PIN_CAP;
    cfg.checkpoint_thread = true;
    cfg.checkpoint_interval_ms = 1;
    ps_backend be;
    mb_backend(st->mb, &be);
    T_OK(ps_open_backend(&be, &cfg, PS_CREATE, &st->s));
    ps_store *s = st->s;

    w->st = st;
    rng_seed(&w->g, st->seed);
    w->next_lsn = 1;

    reader rd[READERS];
    for (u32 i = 0; i < READERS; i++) {
        memset(&rd[i], 0, sizeof(rd[i]));
        rd[i].st = st;
        rd[i].idx = i;
        rd[i].seed = st->seed * 31 + i;
        T_OK(pthread_create(&rd[i].t, NULL, reader_main, &rd[i]));
    }

    const u64 start = t_now_ms();
    const u64 run_ms = (u64) RUN_MS * t_scale();
    struct ps_stats ss;
    for (;;) {
        writer_op(w);
        if (w->commits % 97 == 0) {
            T_OK(ps_checkpoint(s)); /* Exercise callers competing with the checkpointer. */
        }
        if ((w->commits + w->aborts) % 32 != 0) {
            continue;
        }
        ss = get_stats(s);
        T_CHECK(!ss.failed);
        const u64 el = t_now_ms() - start;
        const bool seen = ss.snapshots_cut_off > 0 && ss.cache_misses > 0 &&
            ss.checkpoints_run > 0 && ss.writer_waits > 0 && atomic_load(&st->stale) > 0;
        if (el >= run_ms && (seen || el >= run_ms + EXTRA_MS)) {
            break;
        }
    }
    atomic_store(&st->stop, true);
    reader tot;
    memset(&tot, 0, sizeof(tot));
    for (u32 i = 0; i < READERS; i++) {
        T_OK(pthread_join(rd[i].t, NULL));
        tot.snapshots += rd[i].snapshots;
        tot.reads += rd[i].reads;
        tot.stale += rd[i].stale;
        tot.busy += rd[i].busy;
        tot.cap_hits += rd[i].cap_hits;
    }

    /* Drain checkpoint work before checking for frame leaks. */
    T_OK(ps_checkpoint(s));
    ss = get_stats(s);
    printf(
        "  %.1f s: %llu commits, %llu aborts, %llu snapshots, %llu reads, %llu stale, "
        "%llu busy, %llu at cap; cut off %llu, hits %llu, misses %llu, checkpoints %llu, "
        "waits %llu\n",
        (double) (t_now_ms() - start) / 1000.0,
        (unsigned long long) w->commits,
        (unsigned long long) w->aborts,
        (unsigned long long) tot.snapshots,
        (unsigned long long) tot.reads,
        (unsigned long long) tot.stale,
        (unsigned long long) tot.busy,
        (unsigned long long) tot.cap_hits,
        (unsigned long long) ss.snapshots_cut_off,
        (unsigned long long) ss.cache_hits,
        (unsigned long long) ss.cache_misses,
        (unsigned long long) ss.checkpoints_run,
        (unsigned long long) ss.writer_waits);

    T_CHECK(ss.snapshots_cut_off > 0);
    T_CHECK(ss.cache_misses > 0);
    T_CHECK(ss.checkpoints_run > 0);
    T_CHECK(ss.writer_waits > 0);
    T_CHECK(tot.reads > 0);
    T_CHECK(tot.stale > 0);
    T_CHECK(tot.stale <= ss.snapshots_cut_off);
    T_CHECK(ss.cache_hits + ss.cache_misses >= tot.reads);

    T_CHECK(!ss.failed);
    T_EQ(ss.checkpoints_failed, 0);
    T_EQ(ss.snapshots_open, 0);
    T_EQ(ss.frames_pinned, 0);
    T_EQ(ss.frames_retired, 0);
    T_EQ(ss.frames_dirty, 0);
    T_EQ(ss.num_groups, 1);
    T_EQ(ss.frames_total, FRAMES);
    T_EQ(ss.frames_free + ss.frames_evictable + ss.num_groups, ss.frames_total);
    T_EQ(ss.committed_lsn, w->last_committed);
    T_EQ(ss.durable_lsn, w->last_committed);
    T_EQ(ss.allocated_pages, w->nlive);

    check_final(st, w, s);
    ps_close(s);
    T_OK(mb_open(st->mb, FRAMES, 0, &st->s));
    s = st->s;
    check_final(st, w, s);
    T_OK(ps_op_begin(s, w->next_lsn));
    u32 id = 0;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id, next_alloc_id(w, 0));
    ps_op_abort(s);
    ps_close(s);

    mb_destroy(st->mb);
    for (u32 i = 0; i < ID_MAX; i++) {
        free(st->ids[i].v);
    }
    T_EQ(pthread_rwlock_destroy(&st->mu), 0);
    free(w);
    free(st);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_writer_readers_checkpoint);
    return t_done();
}
