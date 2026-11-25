/*
 * Replay one workload with different cache sizes and checkpoint schedules.
 * Page bytes and allocation IDs must agree even when eviction order and
 * background checkpoint timing differ.
 */

#include "mem_backend.h"
#include "model.h"
#include "rng.h"
#include "test.h"
#include "workload.h"

#include "pagestore/backend.h"
#include "pagestore/pagestore.h"

#include <errno.h>
#include <stdlib.h>

#define PS 4096u
#define OPS 1500u
#define NSTORES 3u

typedef struct store_cfg {
    u32 frames;
    u32 ckpt_every; /* 0 disables periodic calls to ps_checkpoint. */
    bool thread;
    u32 interval_ms;
} store_cfg;

static const store_cfg CFGS[NSTORES] = {
    {32, 1, false, 0},
    {1024, 13, false, 0},
    {256, 0, true, 1},
};

typedef struct dstore {
    store_cfg cfg;
    mem_backend *mb;
    ps_store *s;
} dstore;

static void open_one(dstore *d, u32 flags)
{
    ps_config cfg = mb_config(d->cfg.frames);
    cfg.checkpoint_thread = d->cfg.thread;
    cfg.checkpoint_interval_ms = d->cfg.interval_ms;
    ps_backend be;
    mb_backend(d->mb, &be);
    T_OK(ps_open_backend(&be, &cfg, flags, &d->s));
    T_EQ(ps_page_size(d->s), PS);
}

/* With no checkpoint thread, EAGAIN asks the test to checkpoint before retrying. */
static void apply(ps_store *s, const w_op *op)
{
    for (u32 tries = 0;; tries++) {
        const int rv = w_apply_store(s, op);
        if (rv == 0) {
            return;
        }
        T_ERR(rv, EAGAIN);
        T_CHECK(tries < 3);
        T_OK(ps_checkpoint(s));
    }
}

static int snap_read(ps_store *s, ps_snapshot *sn, ps_page_id id, const ps_page **p)
{
    for (u32 tries = 0;; tries++) {
        const int rv = ps_snapshot_read(sn, id, p);
        if (rv != EAGAIN || tries == 3) {
            return rv;
        }
        T_OK(ps_checkpoint(s));
    }
}

/* Compare full in-memory pages, including the header, at a common LSN. */
static void compare_memory(dstore *d, const model *m)
{
    ps_snapshot *sn[NSTORES];
    const u64 lsn = model_last_lsn(m);
    for (u32 k = 0; k < NSTORES; k++) {
        T_EQ(ps_committed_lsn(d[k].s).raw, lsn);
        T_OK(ps_snapshot_open(d[k].s, &sn[k]));
        T_EQ(ps_snapshot_lsn(sn[k]).raw, lsn);
        struct ps_stats st;
        ps_stats(d[k].s, &st);
        T_EQ(st.allocated_pages, model_allocated_at(m, lsn, NULL, 0));
    }
    const u32 n = model_allocated_at(m, lsn, NULL, 0);
    ps_page_id *ids = t_malloc(((usize) n + 1) * sizeof(*ids));
    model_allocated_at(m, lsn, ids, n);
    for (u32 i = 0; i < n; i++) {
        const ps_page *p[NSTORES];
        for (u32 k = 0; k < NSTORES; k++) {
            T_OK(snap_read(d[k].s, sn[k], ids[i], &p[k]));
        }
        T_EQ(p[0]->id.raw, ids[i].raw);
        for (u32 k = 1; k < NSTORES; k++) {
            T_MEMEQ(p[k], p[0], PS);
        }
        for (u32 k = 0; k < NSTORES; k++) {
            ps_snapshot_unpin(sn[k], ids[i]);
        }
    }
    free(ids);
    for (u32 k = 0; k < NSTORES; k++) {
        ps_snapshot_close(sn[k]);
        w_check_store(d[k].s, m);
    }
}

/* Skips seq because the stores checkpoint at different rates. */
static void compare_durable(dstore *d, const model *m)
{
    const u64 lsn = model_last_lsn(m);
    const ps_backend_info i0 = mb_info(d[0].mb);
    T_EQ(i0.page_size, PS);
    T_EQ(i0.durable_lsn.raw, lsn);
    T_EQ(i0.num_groups, model_num_groups(m));
    for (u32 k = 1; k < NSTORES; k++) {
        const ps_backend_info ik = mb_info(d[k].mb);
        T_EQ(ik.page_size, i0.page_size);
        T_EQ(ik.num_groups, i0.num_groups);
        T_EQ(ik.durable_lsn.raw, i0.durable_lsn.raw);
    }
    const u32 gs = model_group_size(m);
    for (u32 g = 0; g < i0.num_groups; g++) {
        const u8 *b0 = mb_page(d[0].mb, ps_page_id_from(g * gs));
        T_CHECK(b0 != NULL);
        for (u32 k = 1; k < NSTORES; k++) {
            const u8 *bk = mb_page(d[k].mb, ps_page_id_from(g * gs));
            T_CHECK(bk != NULL);
            T_MEMEQ(bk, b0, PS);
        }
    }
    const u32 n = model_allocated_at(m, lsn, NULL, 0);
    ps_page_id *ids = t_malloc(((usize) n + 1) * sizeof(*ids));
    model_allocated_at(m, lsn, ids, n);
    for (u32 i = 0; i < n; i++) {
        const u8 *p0 = mb_page(d[0].mb, ids[i]);
        T_CHECK(p0 != NULL);
        for (u32 k = 1; k < NSTORES; k++) {
            const u8 *pk = mb_page(d[k].mb, ids[i]);
            T_CHECK(pk != NULL);
            T_MEMEQ(pk, p0, PS);
        }
    }
    free(ids);
}

static void run_seed(u64 seed)
{
    rng r;
    rng_seed(&r, seed);
    model *m = model_create(PS);

    /* Generate once so store behavior cannot steer the workload. */
    w_op *ops = t_malloc((usize) OPS * sizeof(*ops));
    u32 nalloc = 0;
    for (u32 i = 0; i < OPS; i++) {
        const u64 lsn = model_last_lsn(m) + 1 + rng_below(&r, 3);
        w_gen(&r, m, lsn, 1 + (u32) rng_below(&r, W_MAX_ACTIONS), &ops[i]);
        T_CHECK(w_op_frames_bound(m, &ops[i]) <= W_OP_FRAMES_MAX);
        w_apply_model(m, &ops[i]);
        for (u32 j = 0; j < ops[i].n; j++) {
            nalloc += ops[i].a[j].kind == W_ALLOC;
        }
    }
    T_CHECK(nalloc > 0);

    dstore d[NSTORES];
    for (u32 k = 0; k < NSTORES; k++) {
        d[k].cfg = CFGS[k];
        d[k].mb = mb_create();
        open_one(&d[k], PS_CREATE);
    }

    /* w_apply_store compares each allocation with the ID recorded by the model. */
    for (u32 k = 0; k < NSTORES; k++) {
        for (u32 i = 0; i < OPS; i++) {
            apply(d[k].s, &ops[i]);
            if (d[k].cfg.ckpt_every != 0 && (i + 1) % d[k].cfg.ckpt_every == 0) {
                T_OK(ps_checkpoint(d[k].s));
            }
        }
    }
    struct ps_stats st;
    ps_stats(d[0].s, &st);
    T_CHECK(st.cache_misses > 0);
    compare_memory(d, m);

    for (u32 k = 0; k < NSTORES; k++) {
        T_OK(ps_checkpoint(d[k].s));
        T_EQ(ps_durable_lsn(d[k].s).raw, model_last_lsn(m));
    }
    compare_durable(d, m);

    for (u32 k = 0; k < NSTORES; k++) {
        ps_close(d[k].s);
        open_one(&d[k], 0);
        T_EQ(ps_committed_lsn(d[k].s).raw, model_last_lsn(m));
        T_EQ(ps_durable_lsn(d[k].s).raw, model_last_lsn(m));
    }
    compare_memory(d, m);
    compare_durable(d, m);

    for (u32 k = 0; k < NSTORES; k++) {
        ps_close(d[k].s);
        mb_destroy(d[k].mb);
    }
    free(ops);
    model_destroy(m);
}

static void test_same_images_and_ids(void)
{
    const u32 seeds = t_scale();
    for (u32 seed = 1; seed <= seeds; seed++) {
        run_seed(0xD5EED000ull + seed);
    }
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_same_images_and_ids);
    return t_done();
}
