/*
 * Run the same seeds with two cache sizes so snapshot and allocation
 * checks also exercise eviction, cut-offs and checkpoint retries.
 */

#include "allocator.h"
#include "mem_backend.h"
#include "model.h"
#include "rng.h"
#include "store.h"
#include "test.h"
#include "workload.h"

#include "pagestore/pagestore.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PS 4096u
#define HDR PS_PAGE_HEADER_SIZE
#define FRAMES 256u
#define SMALL_FRAMES 64u
#define STEPS 2000u
#define MAX_SNAPS 16u
#define HELD 2u
#define NPROBE 8u
#define MAX_GROUPS 64u

typedef struct snap_rec {
    ps_snapshot *sn;
    u64 lsn;
    u32 nheld;
    ps_page_id held_id[HELD];
    const ps_page *held[HELD];
} snap_rec;

/* Outcome counters over every run, to show the interesting paths were taken. */
typedef struct tally {
    u64 ops;
    u64 aborted;
    u64 reopens;
    u64 failures;
    u64 stale; /* verified snapshots that had been cut off */
    u64 misuse_einval;
    u64 misuse_ebadmsg;
    u64 misuse_bytes;
    u64 failed_eio; /* snapshot reads on a failed store that needed a page-in */
    u64 eagain; /* EAGAIN answered with a checkpoint */
} tally;

typedef struct run {
    rng r;
    mem_backend *mb;
    ps_store *s;
    model *m;
    u32 frames;
    u32 max_snaps; /* remembered snapshots: max_snaps * held stays below the pin budget */
    u32 held;
    bool failed;
    snap_rec snaps[MAX_SNAPS];
    u32 nsnaps;
    tally *t;
} run;

static void open_store(run *t, u32 flags)
{
    T_OK(mb_open(t->mb, t->frames, flags, &t->s));
    t->failed = false;
}

static void checkpoint(run *t)
{
    T_OK(ps_checkpoint(t->s));
    T_EQ(ps_durable_lsn(t->s).raw, ps_committed_lsn(t->s).raw);
    model_durable(t->m, ps_durable_lsn(t->s).raw);
}

static u64 next_lsn(run *t)
{
    T_EQ(ps_committed_lsn(t->s).raw, model_last_lsn(t->m));
    return ps_committed_lsn(t->s).raw + 1 + rng_below(&t->r, 3);
}

static void check_page(const model *m, const ps_page *p, ps_page_id id, const model_page *mp)
{
    T_EQ(p->id.raw, id.raw);
    T_EQ(p->lsn.raw, mp->lsn);
    T_EQ(p->flags, 0);
    T_EQ(p->checksum, 0);
    T_EQ(p->magic, 0);
    T_MEMEQ((const u8 *) p + HDR, mp->caller, model_caller_size(m));
}

/* w_apply_store_begin. Without a checkpoint thread, answer EAGAIN with a checkpoint. */
static void begin_op(run *t, const w_op *op)
{
    int rv = 0;
    for (u32 tries = 0;; tries++) {
        rv = w_apply_store_begin(t->s, op);
        if (rv != EAGAIN) {
            break;
        }
        T_CHECK(tries < 3);
        t->t->eagain++;
        checkpoint(t);
    }
    T_OK(rv);
    ps_pressure p;
    ps_op_pressure(t->s, &p);
    const u32 bound = w_op_frames_bound(t->m, op);
    T_CHECK(bound <= W_OP_FRAMES_MAX);
    T_CHECK(p.op_frames <= bound);
    T_CHECK(p.op_frames <= p.op_limit);
}

/* Abort the probe so allocation checks leave the operation history alone. */
static void probe_ids(run *t)
{
    ps_page_id want[3];
    model_next_ids(t->m, 3, want);
    for (u32 tries = 0;; tries++) {
        T_CHECK(tries < 3);
        T_OK(ps_op_begin(t->s, ps_lsn_next(ps_committed_lsn(t->s))));
        int rv = 0;
        for (u32 k = 0; k < 3 && rv == 0; k++) {
            ps_page_id id = PS_PAGE_ID_NONE;
            ps_page *p = NULL;
            rv = ps_alloc(t->s, &id, &p);
            if (rv == 0) {
                T_EQ(id.raw, want[k].raw);
            }
        }
        ps_op_abort(t->s);
        if (rv != EAGAIN) {
            T_OK(rv);
            return;
        }
        t->t->eagain++;
        checkpoint(t);
    }
}

static snap_rec take_snap(run *t, u32 i)
{
    T_CHECK(i < t->nsnaps);
    const snap_rec sr = t->snaps[i];
    t->snaps[i] = t->snaps[--t->nsnaps];
    return sr;
}

/* May close another remembered snapshot to free frames before retrying. */
static int snap_read(run *t, snap_rec *sr, ps_page_id id, const ps_page **p)
{
    for (u32 tries = 0;; tries++) {
        T_CHECK(tries < 4 + MAX_SNAPS);
        const int rv = ps_snapshot_read(sr->sn, id, p);
        if (rv == EAGAIN && !t->failed) {
            t->t->eagain++;
            checkpoint(t);
            continue;
        }
        if (rv == ENOBUFS) {
            /* The test leaves room in the pin budget. Drop an old snapshot to free frames. */
            T_CHECK(t->nsnaps > 0);
            u32 oldest = 0;
            for (u32 i = 1; i < t->nsnaps; i++) {
                if (t->snaps[i].lsn < t->snaps[oldest].lsn) {
                    oldest = i;
                }
            }
            ps_snapshot_close(take_snap(t, oldest).sn);
            continue;
        }
        return rv;
    }
}

/* Returns PS_PAGE_ID_NONE if the sampled user IDs are all allocated at lsn. */
static ps_page_id pick_unallocated(run *t, u64 lsn)
{
    const u32 g = model_group_size(t->m);
    ps_page_id next = PS_PAGE_ID_NONE;
    model_next_ids(t->m, 1, &next);
    const u32 hi = next.raw + 16;
    for (u32 tries = 0; tries < 32; tries++) {
        const ps_page_id id = ps_page_id_from(1 + (u32) rng_below(&t->r, hi));
        if (id.raw % g != 0 && !model_page_at(t->m, lsn, id, NULL)) {
            return id;
        }
    }
    return PS_PAGE_ID_NONE;
}

/* Misuse read of id at sr's LSN: one of the outcomes model_misuse_ok allows. */
static int misuse_read(run *t, snap_rec *sr, ps_page_id id)
{
    const ps_page *p = NULL;
    const int rv = snap_read(t, sr, id, &p);
    if (rv == ESTALE) {
        return rv;
    }
    if (rv == EIO && t->failed) {
        t->t->failed_eio++;
        return 0;
    }
    /* A checkpoint during the read may have changed the data file. */
    const bool ok = rv == 0 ?
        model_misuse_ok(t->m, sr->lsn, id, 0, p->lsn.raw, (const u8 *) p + HDR) :
        model_misuse_ok(t->m, sr->lsn, id, rv, 0, NULL);
    if (!ok) {
        T_FAIL(
            "misuse read of %u at LSN %llu: rv %d, page lsn %llu",
            id.raw,
            (unsigned long long) sr->lsn,
            rv,
            rv == 0 ? (unsigned long long) p->lsn.raw : 0ull);
    }
    if (rv == EINVAL) {
        t->t->misuse_einval++;
    } else if (rv == EBADMSG) {
        t->t->misuse_ebadmsg++;
    } else {
        T_EQ(p->id.raw, id.raw);
        T_EQ(p->flags, 0);
        T_EQ(p->checksum, 0);
        ps_snapshot_unpin(sr->sn, id);
        t->t->misuse_bytes++;
    }
    return 0;
}

/* Pins must stay valid across new reads, even if the snapshot gets cut off. */
static void verify_snap(run *t, snap_rec *sr)
{
    const model *m = t->m;
    for (u32 i = 0; i < sr->nheld; i++) {
        model_page mp;
        T_CHECK(model_page_at(m, sr->lsn, sr->held_id[i], &mp));
        check_page(m, sr->held[i], sr->held_id[i], &mp);
    }
    const u32 n = model_allocated_at(m, sr->lsn, NULL, 0);
    ps_page_id *ids = malloc(((usize) n + 1) * sizeof(*ids));
    T_CHECK(ids != NULL);
    T_EQ(model_allocated_at(m, sr->lsn, ids, n), n);

    bool stale = false;
    for (u32 i = 0; i < n && !stale; i++) {
        const ps_page_id id = ids[i];
        const ps_page *p = NULL;
        const int rv = snap_read(t, sr, id, &p);
        if (rv == ESTALE) {
            stale = true;
            break;
        }
        if (rv == EIO && t->failed) {
            t->t->failed_eio++;
            continue;
        }
        T_OK(rv);
        model_page mp;
        T_CHECK(model_page_at(m, sr->lsn, id, &mp));
        check_page(m, p, id, &mp);
        u32 h = 0;
        while (h < sr->nheld && !ps_page_id_eq(sr->held_id[h], id)) {
            h++;
        }
        if (h < sr->nheld) {
            T_CHECK(p == sr->held[h]);
        } else {
            ps_snapshot_unpin(sr->sn, id);
        }
    }
    for (u32 k = 0; k < NPROBE && !stale; k++) {
        const u32 x = (u32) rng_below(&t->r, 8);
        ps_page_id id = PS_PAGE_ID_NONE;
        if (x == 0) {
            id = ps_page_id_from(0);
        } else if (x == 1) {
            id = ps_page_id_from(UINT32_MAX);
        } else if (x == 2) {
            id = ps_page_id_from(model_group_size(m) * (1 + (u32) rng_below(&t->r, 3)));
        } else {
            id = pick_unallocated(t, sr->lsn);
            if (ps_page_id_is_none(id)) {
                continue;
            }
        }
        stale = misuse_read(t, sr, id) == ESTALE;
    }
    if (stale) {
        const ps_page *p = NULL;
        T_ERR(ps_snapshot_read(sr->sn, n > 0 ? ids[0] : ps_page_id_from(1), &p), ESTALE);
        struct ps_stats st;
        ps_stats(t->s, &st);
        T_CHECK(st.snapshots_cut_off > 0);
        t->t->stale++;
    }
    for (u32 i = 0; i < sr->nheld; i++) {
        model_page mp;
        T_CHECK(model_page_at(m, sr->lsn, sr->held_id[i], &mp));
        check_page(m, sr->held[i], sr->held_id[i], &mp);
    }
    free(ids);
}

static void verify_close(run *t, u32 i)
{
    snap_rec sr = take_snap(t, i);
    verify_snap(t, &sr);
    ps_snapshot_close(sr.sn);
}

static void close_snaps(run *t, bool verify)
{
    while (t->nsnaps > 0) {
        if (verify) {
            verify_close(t, t->nsnaps - 1);
        } else {
            ps_snapshot_close(take_snap(t, t->nsnaps - 1).sn);
        }
    }
}

static void step_op(run *t)
{
    w_op op;
    w_gen(&t->r, t->m, next_lsn(t), 1 + (u32) rng_below(&t->r, W_MAX_ACTIONS), &op);
    begin_op(t, &op);
    ps_op_commit(t->s);
    w_apply_model(t->m, &op);
    T_EQ(ps_committed_lsn(t->s).raw, op.lsn);
    t->t->ops++;
}

/* Use as the final call before abort so the error cannot affect later checks. */
static void misuse_in_op(run *t, const w_op *op)
{
    const u32 g = model_group_size(t->m);
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page_id freed = PS_PAGE_ID_NONE;
    for (u32 i = 0; i < op->n; i++) {
        if (op->a[i].kind == W_FREE) {
            freed = op->a[i].id;
        }
    }
    const u32 x = (u32) rng_below(&t->r, 6);
    if (x == 0) {
        id = ps_page_id_from(g * (u32) rng_below(&t->r, 3));
    } else if (x == 1) {
        id = ps_page_id_from(UINT32_MAX);
    } else if (x == 2 && !ps_page_id_is_none(freed)) {
        id = freed;
    } else if (x == 3) {
        T_ERR(ps_op_begin(t->s, ps_lsn_from(op->lsn + 1)), EINVAL);
        return;
    } else {
        for (u32 tries = 0; tries < 32 && ps_page_id_is_none(id); tries++) {
            const ps_page_id c = ps_page_id_from(1 + (u32) rng_below(&t->r, W_LIVE_TARGET * 2));
            if (c.raw % g != 0 && !model_op_page(t->m, c, NULL)) {
                id = c;
            }
        }
        if (ps_page_id_is_none(id)) {
            return;
        }
    }
    T_CHECK(!model_op_page(t->m, id, NULL));
    const u32 call = (u32) rng_below(&t->r, 3);
    if (call == 0) {
        const ps_page *p = NULL;
        T_ERR(ps_read(t->s, id, &p), EINVAL);
    } else if (call == 1) {
        ps_page *p = NULL;
        T_ERR(ps_write(t->s, id, &p), EINVAL);
    } else {
        T_ERR(ps_free(t->s, id), EINVAL);
    }
}

/* An operation that is aborted: the writer's view inside it, then a misuse probe. */
static void step_abort(run *t)
{
    const u64 before = ps_committed_lsn(t->s).raw;
    w_op op;
    w_gen(&t->r, t->m, next_lsn(t), 1 + (u32) rng_below(&t->r, W_MAX_ACTIONS), &op);
    begin_op(t, &op);
    w_apply_model_begin(t->m, &op);
    for (u32 i = 0; i < op.n; i++) {
        const w_action *a = &op.a[i];
        model_page mp;
        if (a->kind == W_FREE || !model_op_page(t->m, a->id, &mp)) {
            continue;
        }
        const ps_page *p = NULL;
        T_OK(ps_read(t->s, a->id, &p));
        check_page(t->m, p, a->id, &mp);
        T_EQ(p->lsn.raw, op.lsn);
    }
    misuse_in_op(t, &op);
    ps_op_abort(t->s);
    model_op_abort(t->m);
    T_EQ(ps_committed_lsn(t->s).raw, before);
    t->t->aborted++;
}

static void step_snap_open(run *t)
{
    if (t->nsnaps == t->max_snaps) {
        verify_close(t, (u32) rng_below(&t->r, t->nsnaps));
    }
    /* Keep sr off the list while snap_read can remove an older entry. */
    snap_rec sr;
    memset(&sr, 0, sizeof(sr));
    T_OK(ps_snapshot_open(t->s, &sr.sn));
    sr.lsn = ps_snapshot_lsn(sr.sn).raw;
    T_EQ(sr.lsn, ps_committed_lsn(t->s).raw);

    const u32 n = model_allocated_at(t->m, sr.lsn, NULL, 0);
    ps_page_id *ids = malloc(((usize) n + 1) * sizeof(*ids));
    T_CHECK(ids != NULL);
    model_allocated_at(t->m, sr.lsn, ids, n);
    for (u32 k = 0; k < t->held && n > 0; k++) {
        const ps_page_id id = ids[rng_below(&t->r, n)];
        if (k == 1 && ps_page_id_eq(sr.held_id[0], id)) {
            break;
        }
        const ps_page *p = NULL;
        T_OK(snap_read(t, &sr, id, &p));
        model_page mp;
        T_CHECK(model_page_at(t->m, sr.lsn, id, &mp));
        check_page(t->m, p, id, &mp);
        sr.held_id[sr.nheld] = id;
        sr.held[sr.nheld] = p;
        sr.nheld++;
    }
    free(ids);
    T_CHECK(t->nsnaps < t->max_snaps);
    t->snaps[t->nsnaps++] = sr;
}

static void reopen(run *t, bool ckpt_first)
{
    close_snaps(t, false);
    if (ckpt_first) {
        checkpoint(t);
    }
    ps_close(t->s);
    open_store(t, 0);
    const u64 d = ps_durable_lsn(t->s).raw;
    T_EQ(ps_committed_lsn(t->s).raw, d);
    T_CHECK(model_has_lsn(t->m, d));
    model_truncate(t->m, d);
    T_EQ(model_last_lsn(t->m), d);
    w_check_store(t->s, t->m);
    model_durable(t->m, ps_durable_lsn(t->s).raw);
    t->t->reopens++;
}

/*
 * A fatal checkpoint can leave an open operation able to commit in
 * memory. Reopen must still land at the previous durable LSN.
 */
static void step_fail(run *t)
{
    w_op op;
    w_gen(&t->r, t->m, next_lsn(t), 1 + (u32) rng_below(&t->r, W_MAX_ACTIONS), &op);
    begin_op(t, &op);
    if (ps_committed_lsn(t->s).raw == ps_durable_lsn(t->s).raw) {
        /* A checkpoint with no work would leave the fault armed. */
        ps_op_commit(t->s);
        w_apply_model(t->m, &op);
        t->t->ops++;
        return;
    }
    const u64 durable = ps_durable_lsn(t->s).raw;
    mb_fail(t->mb, MB_COMMIT_FATAL, EIO, 1);
    T_ERR(ps_checkpoint(t->s), EIO);
    t->failed = true;
    struct ps_stats st;
    ps_stats(t->s, &st);
    T_CHECK(st.failed);
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_ERR(ps_alloc(t->s, &id, &p), EIO);
    ps_op_commit(t->s);
    T_EQ(ps_committed_lsn(t->s).raw, op.lsn);
    T_EQ(ps_durable_lsn(t->s).raw, durable);
    T_ERR(ps_op_begin(t->s, ps_lsn_from(op.lsn + 1)), EIO);
    ps_snapshot *sn = NULL;
    T_ERR(ps_snapshot_open(t->s, &sn), EIO);
    T_ERR(ps_checkpoint(t->s), EIO);
    close_snaps(t, true);
    ps_close(t->s);
    open_store(t, 0);
    T_EQ(ps_committed_lsn(t->s).raw, durable);
    T_CHECK(model_has_lsn(t->m, durable));
    model_truncate(t->m, durable);
    w_check_store(t->s, t->m);
    t->t->failures++;
}

static void run_seed(u64 seed, u32 frames, tally *ty)
{
    run t;
    memset(&t, 0, sizeof(t));
    t.t = ty;
    t.frames = frames;
    /* Leave room below the 25% pin budget at both cache sizes. */
    t.max_snaps = frames >= 256 ? MAX_SNAPS : MAX_SNAPS / 2;
    t.held = frames >= 256 ? HELD : 1;
    rng_seed(&t.r, seed);
    t.mb = mb_create();
    t.m = model_create(PS);
    open_store(&t, PS_CREATE);
    model_durable(t.m, ps_durable_lsn(t.s).raw);
    for (u32 step = 0; step < STEPS; step++) {
        const u32 x = (u32) rng_below(&t.r, 100);
        bool changed = true;
        if (x < 64) {
            step_op(&t);
        } else if (x < 65) {
            step_fail(&t);
        } else if (x < 75) {
            step_abort(&t);
        } else if (x < 85 || (x < 90 && t.nsnaps == 0)) {
            step_snap_open(&t);
            changed = false;
        } else if (x < 90) {
            verify_close(&t, (u32) rng_below(&t.r, t.nsnaps));
            changed = false;
        } else if (x < 95) {
            checkpoint(&t);
        } else {
            reopen(&t, rng_below(&t.r, 2) == 0);
        }
        if (changed) {
            probe_ids(&t);
        }
    }
    close_snaps(&t, true);
    w_check_store(t.s, t.m);
    ps_close(t.s);
    model_destroy(t.m);
    mb_destroy(t.mb);
}

static void test_model_vs_store(void)
{
    tally ty;
    memset(&ty, 0, sizeof(ty));
    for (u64 seed = 1; seed <= 4u * t_scale(); seed++) {
        run_seed(seed, FRAMES, &ty);
        run_seed(seed, SMALL_FRAMES, &ty);
    }
    fprintf(
        stderr,
        "  ops %llu aborted %llu reopens %llu failures %llu stale %llu eagain %llu\n"
        "  misuse reads: EINVAL %llu EBADMSG %llu bytes %llu; failed-store EIO %llu\n",
        (unsigned long long) ty.ops,
        (unsigned long long) ty.aborted,
        (unsigned long long) ty.reopens,
        (unsigned long long) ty.failures,
        (unsigned long long) ty.stale,
        (unsigned long long) ty.eagain,
        (unsigned long long) ty.misuse_einval,
        (unsigned long long) ty.misuse_ebadmsg,
        (unsigned long long) ty.misuse_bytes,
        (unsigned long long) ty.failed_eio);
    /* Ensure these seeds exercise the error paths as well as successful reads. */
    T_CHECK(ty.failures > 0);
    T_CHECK(ty.stale > 0);
    T_CHECK(ty.eagain > 0);
    T_CHECK(ty.misuse_einval > 0);
    T_CHECK(ty.misuse_ebadmsg > 0);
    T_CHECK(ty.misuse_bytes > 0);
}

/* ---- allocation policy ---- */

/*
 * A cancelled allocation leaves the cursor ahead of its ID, and a freed
 * committed ID stays unavailable until commit.
 */
static void test_model_alloc_matches_policy(void)
{
    for (u64 seed = 1; seed <= 4u * t_scale(); seed++) {
        rng r;
        rng_seed(&r, seed * 7919);
        mem_backend *mb = mb_create();
        model *m = model_create(PS);
        ps_store *s = NULL;
        T_OK(mb_open(mb, FRAMES, PS_CREATE, &s));
        for (u32 step = 0; step < 150; step++) {
            const u64 lsn = ps_committed_lsn(s).raw + 1;
            T_OK(ps_op_begin(s, ps_lsn_from(lsn)));
            model_op_begin(m, lsn);
            const u32 nlive = model_allocated_at(m, model_last_lsn(m), NULL, 0);
            ps_page_id *live = malloc(((usize) nlive + 1) * sizeof(*live));
            T_CHECK(live != NULL);
            model_allocated_at(m, model_last_lsn(m), live, nlive);
            ps_page_id mine[8];
            u32 nmine = 0;
            bool misused = false;
            for (u32 k = 0; k < 8 && !misused; k++) {
                const u32 x = (u32) rng_below(&r, 10);
                if (x < 5 || nlive == 0) {
                    ps_page_id want = PS_PAGE_ID_NONE;
                    model_op_next_ids(m, 1, &want);
                    ps_page_id id = PS_PAGE_ID_NONE;
                    ps_page *p = NULL;
                    T_OK(ps_alloc(s, &id, &p));
                    T_EQ(id.raw, want.raw);
                    T_EQ(model_alloc(m).raw, id.raw);
                    mine[nmine++] = id;
                } else if (x < 7 && nmine > 0) {
                    const u32 i = (u32) rng_below(&r, nmine);
                    T_OK(ps_free(s, mine[i]));
                    model_free(m, mine[i]);
                    mine[i] = mine[--nmine];
                } else {
                    const ps_page_id id = live[rng_below(&r, nlive)];
                    if (model_op_page(m, id, NULL)) {
                        T_OK(ps_free(s, id));
                        model_free(m, id);
                    } else {
                        T_ERR(ps_free(s, id), EINVAL); /* freed earlier in this operation */
                        misused = true;
                    }
                }
            }
            free(live);
            if (misused || rng_below(&r, 3) == 0) {
                ps_op_abort(s);
                model_op_abort(m);
            } else {
                ps_op_commit(s);
                model_op_commit(m);
            }
            struct ps_stats st;
            ps_stats(s, &st);
            T_EQ(st.allocated_pages, model_allocated_at(m, model_last_lsn(m), NULL, 0));
            T_EQ(st.num_groups, model_num_groups(m));
            if (step % 10 == 9) {
                T_OK(ps_checkpoint(s));
                model_durable(m, ps_durable_lsn(s).raw);
            }
            if (step % 50 == 49) {
                ps_close(s);
                T_OK(mb_open(mb, FRAMES, 0, &s));
            }
        }
        w_check_store(s, m);
        ps_close(s);
        model_destroy(m);
        mb_destroy(mb);
    }
}

/* ---- allocator group growth ---- */

struct ps_bitmap_get_ctx {
    u8 **bits; /* committed bitmap caller area per group */
};

static int get_bits(ps_bitmap_get_ctx *ctx, u32 g, const u8 **bits)
{
    *bits = ctx->bits[g];
    return 0;
}

/* Drives ps_allocator directly so operations can cross groups without a cache limit. */
static void test_model_vs_allocator(void)
{
    const u32 g = ps_group_size(PS);
    const usize gbytes = g / 8;
    for (u64 seed = 1; seed <= 2u * t_scale(); seed++) {
        rng r;
        rng_seed(&r, seed * 104729);
        ps_allocator *a = NULL;
        T_OK(ps_allocator_create(PS, &a));
        u8 *bits[MAX_GROUPS] = {0};
        bits[0] = calloc(1, gbytes);
        T_CHECK(bits[0] != NULL);
        bits[0][0] = 1; /* the bitmap page's own bit */
        T_OK(ps_allocator_init_group(a, 0, bits[0]));
        ps_bitmap_get_ctx ctx = {bits};
        model *m = model_create(PS);
        u64 lsn = 0;
        for (u32 step = 0; step < 40; step++) {
            lsn++;
            ps_allocator_op_begin(a);
            model_op_begin(m, lsn);
            const u32 nlive = model_allocated_at(m, model_last_lsn(m), NULL, 0);
            ps_page_id *live = malloc(((usize) nlive + 1) * sizeof(*live));
            T_CHECK(live != NULL);
            model_allocated_at(m, model_last_lsn(m), live, nlive);
            const u32 runs = 1 + (u32) rng_below(&r, 4);
            ps_page_id last_alloc = PS_PAGE_ID_NONE;
            for (u32 k = 0; k < runs; k++) {
                const u32 x = (u32) rng_below(&r, 4);
                if (x < 2) {
                    const u32 n = 1 + (u32) rng_below(&r, g / 4);
                    for (u32 i = 0; i < n; i++) {
                        ps_page_id id = PS_PAGE_ID_NONE;
                        bool created = false;
                        T_OK(ps_allocator_next(a, get_bits, &ctx, &id, &created));
                        T_EQ(model_alloc(m).raw, id.raw);
                        T_EQ(created, id.raw % g == 1 && id.raw / g >= model_num_groups(m));
                        last_alloc = id;
                    }
                } else if (x == 2 && !ps_page_id_is_none(last_alloc)) {
                    bool cancelled = false;
                    T_OK(ps_allocator_free(a, get_bits, &ctx, last_alloc, &cancelled));
                    T_CHECK(cancelled);
                    model_free(m, last_alloc);
                    last_alloc = PS_PAGE_ID_NONE;
                } else if (nlive > 0) {
                    const u32 n = 1 + (u32) rng_below(&r, 64);
                    for (u32 i = 0; i < n; i++) {
                        const ps_page_id id = live[rng_below(&r, nlive)];
                        bool cancelled = false;
                        const int rv = ps_allocator_free(a, get_bits, &ctx, id, &cancelled);
                        if (model_op_page(m, id, NULL)) {
                            T_OK(rv);
                            T_CHECK(!cancelled);
                            model_free(m, id);
                        } else {
                            T_ERR(rv, EINVAL);
                        }
                    }
                }
            }
            free(live);
            if (rng_below(&r, 4) == 0) {
                ps_allocator_op_abort(a);
                model_op_abort(m);
                lsn--;
                continue;
            }
            const u32 *groups = NULL;
            const u32 ng = ps_allocator_op_groups(a, &groups);
            for (u32 i = 0; i < ng; i++) {
                const u32 gi = groups[i];
                T_CHECK(gi < MAX_GROUPS);
                if (ps_allocator_group_is_new(a, gi)) {
                    bits[gi] = calloc(1, gbytes);
                    T_CHECK(bits[gi] != NULL);
                }
                ps_allocator_apply(a, gi, bits[gi]);
            }
            ps_allocator_op_commit(a);
            model_op_commit(m);
            T_EQ(ps_allocator_num_groups(a), model_num_groups(m));
            T_EQ(ps_allocator_allocated(a), model_allocated_at(m, lsn, NULL, 0));
            ps_page_id want[4];
            model_next_ids(m, 4, want);
            ps_allocator_op_begin(a);
            for (u32 i = 0; i < 4; i++) {
                ps_page_id id = PS_PAGE_ID_NONE;
                bool created = false;
                T_OK(ps_allocator_next(a, get_bits, &ctx, &id, &created));
                T_EQ(id.raw, want[i].raw);
            }
            ps_allocator_op_abort(a);
        }
        T_CHECK(model_num_groups(m) >= 3);
        model_destroy(m);
        for (u32 i = 0; i < MAX_GROUPS; i++) {
            free(bits[i]);
        }
        ps_allocator_destroy(a);
    }
}

/* ---- model and workload tests ---- */

static void test_model_basics(void)
{
    model *m = model_create(PS);
    const u32 g = model_group_size(m);
    const u32 cs = model_caller_size(m);
    T_EQ(cs, PS - HDR);
    T_CHECK(model_has_lsn(m, 0));
    T_EQ(model_last_lsn(m), 0);
    T_EQ(model_num_groups(m), 1);
    ps_page_id ids[3];
    model_next_ids(m, 3, ids);
    T_EQ(ids[0].raw, 1);
    T_EQ(ids[2].raw, 3);

    model_op_begin(m, 5);
    T_EQ(model_alloc(m).raw, 1);
    T_EQ(model_alloc(m).raw, 2);
    T_EQ(model_alloc(m).raw, 3);
    const u8 data[4] = {1, 2, 3, 4};
    model_write(m, ps_page_id_from(2), 10, data, 4);
    model_free(m, ps_page_id_from(3));
    T_CHECK(!model_op_page(m, ps_page_id_from(3), NULL));
    T_EQ(model_alloc(m).raw, 4); /* the cursor does not move back */
    model_op_commit(m);
    T_CHECK(model_has_lsn(m, 5));
    T_CHECK(!model_has_lsn(m, 4));

    model_page mp;
    T_CHECK(model_page_at(m, 5, ps_page_id_from(2), &mp));
    T_EQ(mp.lsn, 5);
    T_MEMEQ(mp.caller + 10, data, 4);
    T_CHECK(!model_page_at(m, 4, ps_page_id_from(2), NULL));
    T_CHECK(!model_page_at(m, 5, ps_page_id_from(3), NULL));
    model_next_ids(m, 2, ids);
    T_EQ(ids[0].raw, 3);
    T_EQ(ids[1].raw, 5);

    u8 z[PS - HDR];
    memset(z, 0, sizeof(z));
    T_CHECK(model_misuse_ok(m, 5, ps_page_id_from(3), EINVAL, 0, NULL));
    T_CHECK(model_misuse_ok(m, 5, ps_page_id_from(3), EBADMSG, 0, NULL));
    T_CHECK(!model_misuse_ok(m, 5, ps_page_id_from(3), 0, 5, z));
    T_CHECK(!model_misuse_ok(m, 5, ps_page_id_from(0), EBADMSG, 0, NULL));
    T_CHECK(model_misuse_ok(m, 5, ps_page_id_from(0), EINVAL, 0, NULL));
    T_CHECK(!model_misuse_ok(m, 5, ps_page_id_from(g), 0, 5, z));
    T_CHECK(model_misuse_ok(m, 5, ps_page_id_from(UINT32_MAX), EINVAL, 0, NULL));
    model_durable(m, 5);

    model_op_begin(m, 7);
    model_free(m, ps_page_id_from(2));
    T_EQ(model_alloc(m).raw, 3); /* 2 is free only after commit */
    model_op_commit(m);
    T_CHECK(!model_page_at(m, 7, ps_page_id_from(2), NULL));
    T_CHECK(model_page_at(m, 6, ps_page_id_from(2), &mp));
    /* Freed after the checkpoint at 5: the data file holds the version at 5. */
    T_CHECK(model_misuse_ok(m, 7, ps_page_id_from(2), 0, 5, mp.caller));
    T_CHECK(!model_misuse_ok(m, 7, ps_page_id_from(2), 0, 5, z));
    T_CHECK(!model_misuse_ok(m, 7, ps_page_id_from(2), EBADMSG, 0, NULL));
    T_CHECK(!model_misuse_ok(m, 7, ps_page_id_from(2), EIO, 0, NULL));
    /* At 4 that copy (lsn 5) is newer than the snapshot. */
    T_CHECK(!model_misuse_ok(m, 4, ps_page_id_from(2), 0, 5, mp.caller));
    T_CHECK(model_misuse_ok(m, 4, ps_page_id_from(2), EINVAL, 0, NULL));

    ps_page_id all[8];
    T_EQ(model_allocated_at(m, 7, all, 8), 3);
    T_EQ(all[0].raw, 1);
    T_EQ(all[1].raw, 3);
    T_EQ(all[2].raw, 4);

    model_truncate(m, 5);
    T_EQ(model_last_lsn(m), 5);
    T_CHECK(model_page_at(m, 5, ps_page_id_from(2), NULL));
    model_next_ids(m, 1, ids);
    T_EQ(ids[0].raw, 3);

    model_op_begin(m, 6);
    ps_page_id last = PS_PAGE_ID_NONE;
    for (u32 i = 0; i < g - 4; i++) {
        last = model_alloc(m);
    }
    T_EQ(last.raw, g - 1);
    T_EQ(model_alloc(m).raw, g + 1);
    model_op_commit(m);
    T_EQ(model_num_groups(m), 2);
    model_truncate(m, 5);
    T_EQ(model_num_groups(m), 1);
    model_destroy(m);
}

static void test_workload_codec(void)
{
    rng r;
    rng_seed(&r, 42);
    model *m = model_create(PS);
    for (u32 i = 0; i < 200; i++) {
        w_op op;
        w_gen(&r, m, model_last_lsn(m) + 1 + rng_below(&r, 3), W_MAX_ACTIONS, &op);
        T_CHECK(op.n >= 1 && op.n <= W_MAX_ACTIONS);
        T_CHECK(w_op_frames_bound(m, &op) <= W_OP_FRAMES_MAX);
        u8 buf[W_RECORD_MAX];
        const usize len = w_encode(&op, buf, sizeof(buf));
        T_CHECK(len <= W_RECORD_MAX);
        T_EQ(w_encode(&op, buf, len - 1), len);
        w_op back;
        T_OK(w_decode(buf, len, &back));
        T_EQ(back.lsn, op.lsn);
        T_EQ(back.n, op.n);
        for (u32 k = 0; k < op.n; k++) {
            T_EQ(back.a[k].kind, op.a[k].kind);
            T_EQ(back.a[k].id.raw, op.a[k].id.raw);
            T_EQ(back.a[k].off, op.a[k].off);
            T_EQ(back.a[k].len, op.a[k].len);
            T_EQ(back.a[k].fill, op.a[k].fill);
        }
        T_ERR(w_decode(buf, len - 1, &back), EBADMSG);
        buf[len - 1] ^= 1;
        T_ERR(w_decode(buf, len, &back), EBADMSG);
        w_apply_model(m, &op);
    }
    T_CHECK(model_allocated_at(m, model_last_lsn(m), NULL, 0) > 0);
    u8 a[16];
    w_pattern_fill(9, 100, a, 16);
    for (u32 i = 0; i < 16; i++) {
        T_EQ(a[i], w_pattern(9, 100 + i));
    }
    model_destroy(m);
}

/* After a snapshot loads a freed page's disk copy, the writer still sees it as free. */
static void test_writer_misuse_after_snapshot_load(void)
{
    mem_backend *mb = mb_create();
    ps_store *s = NULL;
    T_OK(mb_open(mb, FRAMES, PS_CREATE, &s));
    T_OK(ps_op_begin(s, ps_lsn_from(1)));
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, 1);
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    T_OK(ps_op_begin(s, ps_lsn_from(2)));
    T_OK(ps_free(s, ps_page_id_from(1)));
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(1)));

    ps_snapshot *sn = NULL;
    T_OK(ps_snapshot_open(s, &sn));
    const ps_page *rp = NULL;
    T_OK(ps_snapshot_read(sn, ps_page_id_from(1), &rp));
    T_EQ(rp->lsn.raw, 1);
    ps_snapshot_close(sn);
    T_CHECK(ps_test_has_entry(s, ps_page_id_from(1)));

    for (u32 call = 0; call < 3; call++) {
        T_OK(ps_op_begin(s, ps_lsn_from(3 + call)));
        if (call == 0) {
            T_ERR(ps_read(s, ps_page_id_from(1), &rp), EINVAL);
        } else if (call == 1) {
            T_ERR(ps_write(s, ps_page_id_from(1), &p), EINVAL);
        } else {
            T_ERR(ps_free(s, ps_page_id_from(1)), EINVAL);
        }
        ps_op_abort(s);
    }
    T_OK(ps_op_begin(s, ps_lsn_from(3)));
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, 1);
    T_EQ(p->lsn.raw, 3);
    ps_op_commit(s);
    ps_close(s);
    mb_destroy(mb);
}

/* A snapshot keeps its cached copy of a freed page after its reuse is checkpointed. */
static void test_snapshot_misuse_older_copy(void)
{
    mem_backend *mb = mb_create();
    model *m = model_create(PS);
    ps_store *s = NULL;
    T_OK(mb_open(mb, FRAMES, PS_CREATE, &s));
    w_op op;
    memset(&op, 0, sizeof(op));
    op.lsn = 1;
    op.n = 2;
    op.a[0].kind = W_ALLOC;
    op.a[0].id = ps_page_id_from(1);
    op.a[1].kind = W_WRITE;
    op.a[1].id = ps_page_id_from(1);
    op.a[1].len = 100;
    op.a[1].fill = 7;
    T_OK(w_apply_store(s, &op));
    w_apply_model(m, &op);
    T_OK(ps_checkpoint(s));
    model_durable(m, 1);
    memset(&op, 0, sizeof(op));
    op.lsn = 2;
    op.n = 1;
    op.a[0].kind = W_FREE;
    op.a[0].id = ps_page_id_from(1);
    T_OK(w_apply_store(s, &op));
    w_apply_model(m, &op);
    T_OK(ps_checkpoint(s)); /* drops the lone free marker */
    model_durable(m, 2);

    ps_snapshot *a = NULL;
    T_OK(ps_snapshot_open(s, &a));
    const ps_page *p = NULL;
    T_OK(ps_snapshot_read(a, ps_page_id_from(1), &p));
    T_CHECK(model_misuse_ok(m, 2, ps_page_id_from(1), 0, p->lsn.raw, (const u8 *) p + HDR));
    ps_snapshot_unpin(a, ps_page_id_from(1));

    memset(&op, 0, sizeof(op));
    op.lsn = 3;
    op.n = 1;
    op.a[0].kind = W_ALLOC;
    op.a[0].id = ps_page_id_from(1);
    T_OK(w_apply_store(s, &op));
    w_apply_model(m, &op);
    T_OK(ps_checkpoint(s));
    model_durable(m, 3);
    T_EQ(((const ps_page *) mb_page(mb, ps_page_id_from(1)))->lsn.raw, 3);

    T_OK(ps_snapshot_read(a, ps_page_id_from(1), &p));
    T_EQ(p->lsn.raw, 1);
    T_CHECK(model_misuse_ok(m, 2, ps_page_id_from(1), 0, p->lsn.raw, (const u8 *) p + HDR));
    ps_snapshot_close(a);
    w_check_store(s, m);
    ps_close(s);
    model_destroy(m);
    mb_destroy(mb);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_model_basics);
    T_RUN(test_workload_codec);
    T_RUN(test_model_vs_allocator);
    T_RUN(test_writer_misuse_after_snapshot_load);
    T_RUN(test_snapshot_misuse_older_copy);
    T_RUN(test_model_alloc_matches_policy);
    T_RUN(test_model_vs_store);
    return t_done();
}
