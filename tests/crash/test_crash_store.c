/*
 * Without a WAL, a crash may recover the last successful checkpoint or
 * a later one that reached disk before it could report success.
 */

#include "fault_vfs.h"
#include "rng.h"
#include "test.h"

#include "pagestore/pagestore.h"

#include <errno.h>
#include <string.h>

#define PS 4096u
#define HDR PS_PAGE_HEADER_SIZE
#define DIR "/s"
#define FRAMES 64u
#define NOPS 40u
#define CKPT_EVERY 5u
#define MAX_PAGES 30u
#define NCKPT (NOPS / CKPT_EVERY)
#define NEXT_ALLOCS 4u

/* The model after one checkpoint, for IDs 1..MAX_PAGES. */
typedef struct image {
    u64 lsn;
    bool ok; /* The checkpoint returned success to the caller. */
    u64 lsn_written[MAX_PAGES + 1]; /* 0 for an unallocated page */
} image;

typedef struct run {
    image images[NCKPT + 1]; /* Slot 0 is the empty formatted store. */
    u32 nimages;
    u64 points; /* Excludes formatting. */
    bool crashed;
} run;

static fault_vfs *fv;

static ps_config crash_config(void)
{
    ps_config cfg;
    ps_config_init(&cfg);
    cfg.page_size = PS;
    cfg.cache_size = (usize) FRAMES * PS;
    cfg.checkpoint_thread = false;
    cfg.vfs = fault_vfs_get(fv);
    return cfg;
}

static void setup(u64 seed)
{
    fv = fault_vfs_create(seed);
}

static void teardown(void)
{
    fault_vfs_destroy(fv);
    fv = NULL;
}

static u8 content_byte(ps_page_id id, u64 lsn, u32 j)
{
    return (u8) (id.raw * 131u + (u32) lsn * 29u + j * 7u + (j >> 8));
}

static void fill(ps_page *p, ps_page_id id, u64 lsn)
{
    u8 *b = (u8 *) p;
    for (u32 j = HDR; j < PS; j++) {
        b[j] = content_byte(id, lsn, j);
    }
}

static void check_content(const ps_page *p, ps_page_id id, u64 lsn)
{
    T_EQ(p->id.raw, id.raw);
    T_EQ(p->lsn.raw, lsn);
    const u8 *b = (const u8 *) p;
    for (u32 j = HDR; j < PS; j++) {
        if (b[j] != content_byte(id, lsn, j)) {
            T_FAIL("page %u lsn %llu: byte %u differs", id.raw, (unsigned long long) lsn, j);
        }
    }
}

/* Returns PS_PAGE_ID_NONE if the model has no allocated pages. */
static ps_page_id pick_allocated(const u64 *lw, rng *r)
{
    u32 n = 0;
    for (u32 id = 1; id <= MAX_PAGES; id++) {
        n += lw[id] != 0;
    }
    if (n == 0) {
        return PS_PAGE_ID_NONE;
    }
    u32 k = (u32) rng_below(r, n);
    for (u32 id = 1; id <= MAX_PAGES; id++) {
        if (lw[id] != 0 && k-- == 0) {
            return ps_page_id_from(id);
        }
    }
    return PS_PAGE_ID_NONE;
}

static u32 count_allocated(const u64 *lw)
{
    u32 n = 0;
    for (u32 id = 1; id <= MAX_PAGES; id++) {
        n += lw[id] != 0;
    }
    return n;
}

/* Publish the model changes only after the store commits. */
static bool run_op(ps_store *s, u64 lsn, u64 *lw, rng *r)
{
    if (ps_op_begin(s, ps_lsn_from(lsn)) != 0) {
        return false;
    }
    u64 next[MAX_PAGES + 1];
    memcpy(next, lw, sizeof(next));
    bool touched[MAX_PAGES + 1] = {false};
    const u32 nact = 1 + (u32) rng_below(r, 3);
    for (u32 a = 0; a < nact; a++) {
        const u32 kind = (u32) rng_below(r, 10);
        int rv = 0;
        if (kind < 5 && count_allocated(next) < MAX_PAGES) {
            ps_page_id id = PS_PAGE_ID_NONE;
            ps_page *p = NULL;
            rv = ps_alloc(s, &id, &p);
            if (rv == 0) {
                T_CHECK(id.raw >= 1 && id.raw <= MAX_PAGES && next[id.raw] == 0);
                fill(p, id, lsn);
                next[id.raw] = lsn;
                touched[id.raw] = true;
            }
        } else if (kind < 8) {
            const ps_page_id id = pick_allocated(next, r);
            if (!ps_page_id_is_none(id)) {
                ps_page *p = NULL;
                rv = ps_write(s, id, &p);
                if (rv == 0) {
                    fill(p, id, lsn);
                    next[id.raw] = lsn;
                    touched[id.raw] = true;
                }
            }
        } else {
            const ps_page_id id = pick_allocated(next, r);
            if (!ps_page_id_is_none(id) && !touched[id.raw]) {
                rv = ps_free(s, id);
                if (rv == 0) {
                    next[id.raw] = 0;
                }
            }
        }
        if (rv != 0) {
            T_CHECK(fault_vfs_crashed(fv));
            ps_op_abort(s);
            return false;
        }
    }
    ps_op_commit(s);
    memcpy(lw, next, sizeof(next));
    return true;
}

/* crash_k is relative to the end of formatting. Use 0 for a dry run. */
static void run_workload(u64 crash_k, run *out)
{
    memset(out, 0, sizeof(*out));
    const ps_config cfg = crash_config();
    ps_store *s = NULL;
    T_OK(ps_open(DIR, &cfg, PS_CREATE, &s));
    const u64 base = fault_vfs_points(fv);
    if (crash_k != 0) {
        fault_vfs_crash_at(fv, base + crash_k);
    }
    out->images[0].lsn = 0;
    out->images[0].ok = true;
    out->nimages = 1;

    rng r;
    rng_seed(&r, 0x5EED);
    u64 lw[MAX_PAGES + 1] = {0};
    for (u64 lsn = 1; lsn <= NOPS; lsn++) {
        if (!run_op(s, lsn, lw, &r)) {
            break;
        }
        if (lsn % CKPT_EVERY == 0) {
            image *im = &out->images[out->nimages++];
            im->lsn = lsn;
            memcpy(im->lsn_written, lw, sizeof(lw));
            const int rv = ps_checkpoint(s);
            im->ok = rv == 0;
            if (rv != 0) {
                T_CHECK(fault_vfs_crashed(fv));
            }
        }
    }
    out->crashed = fault_vfs_crashed(fv);
    out->points = fault_vfs_points(fv) - base;
    ps_close(s);
}

/* Reject a recovered LSN older than the last acknowledged checkpoint. */
static const image *expected_at(const run *rn, u64 L)
{
    u32 last_ok = 0;
    for (u32 i = 0; i < rn->nimages; i++) {
        if (rn->images[i].ok) {
            last_ok = i;
        }
    }
    for (u32 i = last_ok; i < rn->nimages; i++) {
        if (rn->images[i].lsn == L) {
            return &rn->images[i];
        }
    }
    T_FAIL(
        "durable lsn %llu is no allowed checkpoint (last ok %llu)",
        (unsigned long long) L,
        (unsigned long long) rn->images[last_ok].lsn);
}

/* Checks the reopened store against im, then that it keeps working. */
static void verify(const image *im)
{
    const ps_config cfg = crash_config();
    ps_store *s = NULL;
    T_OK(ps_open(DIR, &cfg, 0, &s));
    const u64 L = ps_durable_lsn(s).raw;
    T_EQ(L, im->lsn);
    T_EQ(ps_committed_lsn(s).raw, L);
    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.allocated_pages, count_allocated(im->lsn_written));
    T_CHECK(!st.failed);

    ps_snapshot *snap = NULL;
    T_OK(ps_snapshot_open(s, &snap));
    for (u32 i = 1; i <= MAX_PAGES; i++) {
        if (im->lsn_written[i] != 0) {
            const ps_page_id id = ps_page_id_from(i);
            const ps_page *p = NULL;
            T_OK(ps_snapshot_read(snap, id, &p));
            check_content(p, id, im->lsn_written[i]);
            ps_snapshot_unpin(snap, id);
        }
    }
    ps_snapshot_close(snap);

    /* Abort the allocation probe so the follow-up starts from the recovered image. */
    T_OK(ps_op_begin(s, ps_lsn_from(L + 1)));
    u32 want = 1;
    for (u32 n = 0; n < NEXT_ALLOCS; n++) {
        while (want <= MAX_PAGES && im->lsn_written[want] != 0) {
            want++;
        }
        ps_page_id id = PS_PAGE_ID_NONE;
        ps_page *p = NULL;
        T_OK(ps_alloc(s, &id, &p));
        T_EQ(id.raw, want);
        want++;
    }
    ps_op_abort(s);

    ps_page_id nid = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_op_begin(s, ps_lsn_from(L + 1)));
    T_OK(ps_alloc(s, &nid, &p));
    fill(p, nid, L + 1);
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    ps_close(s);
    T_OK(ps_open(DIR, &cfg, 0, &s));
    T_EQ(ps_durable_lsn(s).raw, L + 1);
    T_OK(ps_snapshot_open(s, &snap));
    const ps_page *rp = NULL;
    T_OK(ps_snapshot_read(snap, nid, &rp));
    check_content(rp, nid, L + 1);
    for (u32 i = 1; i <= MAX_PAGES; i++) {
        if (im->lsn_written[i] != 0) {
            const ps_page_id id = ps_page_id_from(i);
            T_OK(ps_snapshot_read(snap, id, &rp));
            check_content(rp, id, im->lsn_written[i]);
            ps_snapshot_unpin(snap, id);
        }
    }
    ps_snapshot_close(snap);
    ps_close(s);
}

/* k counts fault points after open. Returns the durable LSN after reboot. */
static u64 crash_ckpt_once(u64 k, fault_mode mode, u64 seed)
{
    setup(seed);
    run rn;
    run_workload(k, &rn);
    T_CHECK(rn.crashed);
    fault_vfs_reboot(fv, mode);
    const ps_config cfg = crash_config();
    ps_store *s = NULL;
    T_OK(ps_open(DIR, &cfg, 0, &s));
    const u64 L = ps_durable_lsn(s).raw;
    ps_close(s);
    verify(expected_at(&rn, L));
    teardown();
    return L;
}

static void test_crash_during_checkpoints(void)
{
    setup(1);
    run dry;
    run_workload(0, &dry);
    T_CHECK(!dry.crashed);
    T_EQ(dry.nimages, NCKPT + 1);
    for (u32 i = 0; i < dry.nimages; i++) {
        T_CHECK(dry.images[i].ok);
    }
    fault_vfs_reboot(fv, FAULT_RANDOM_UNSYNCED);
    verify(&dry.images[NCKPT]);
    teardown();

    const u64 total = dry.points;
    T_CHECK(total > 10 * NCKPT);
    u64 stride = total / (200u * (u64) t_scale());
    if (stride == 0) {
        stride = 1;
    }
    /* Bit L / CKPT_EVERY records a recovery at LSN L. */
    u32 seen = 0;
    u64 k = 1;
    for (;;) {
        seen |= 1u << (crash_ckpt_once(k, FAULT_DROP_UNSYNCED, k) / CKPT_EVERY);
        for (u32 i = 0; i < t_scale(); i++) {
            const u64 seed = k * 7919u + i + 1;
            seen |= 1u << (crash_ckpt_once(k, FAULT_RANDOM_UNSYNCED, seed) / CKPT_EVERY);
        }
        if (k == total) {
            break;
        }
        k = k + stride < total ? k + stride : total;
    }
    T_CHECK((seen & 1u) != 0);
    T_CHECK((seen & (1u << (NCKPT - 1))) != 0);
    T_CHECK((seen & (1u << NCKPT)) != 0);
}

/* Returns false once k is past the last point in format. */
static bool crash_format_once(u64 k, fault_mode mode, u64 seed)
{
    setup(seed);
    const ps_config cfg = crash_config();
    ps_store *s = NULL;
    fault_vfs_crash_at(fv, fault_vfs_points(fv) + k);
    const int rv = ps_open(DIR, &cfg, PS_CREATE, &s);
    const bool crashed = fault_vfs_crashed(fv);
    if (rv == 0) {
        /*
         * Format can succeed even if journal truncation fails after
         * the header is durable.
         */
        ps_close(s);
        s = NULL;
    } else {
        T_CHECK(crashed);
        T_CHECK(s == NULL);
    }
    fault_vfs_reboot(fv, mode);

    const int rv2 = ps_open(DIR, &cfg, 0, &s);
    if (rv == 0) {
        T_OK(rv2);
    }
    if (rv2 == 0) {
        T_EQ(ps_committed_lsn(s).raw, 0);
        T_EQ(ps_durable_lsn(s).raw, 0);
        ps_close(s);
    } else {
        T_ERR(rv2, ENOENT);
    }
    s = NULL;

    T_OK(ps_open(DIR, &cfg, PS_CREATE, &s));
    T_EQ(ps_committed_lsn(s).raw, 0);
    T_OK(ps_op_begin(s, ps_lsn_from(1)));
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    T_EQ(id.raw, 1);
    fill(p, id, 1);
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    ps_close(s);
    T_OK(ps_open(DIR, &cfg, 0, &s));
    T_EQ(ps_durable_lsn(s).raw, 1);
    ps_snapshot *snap = NULL;
    T_OK(ps_snapshot_open(s, &snap));
    const ps_page *rp = NULL;
    T_OK(ps_snapshot_read(snap, id, &rp));
    check_content(rp, id, 1);
    ps_snapshot_close(snap);
    ps_close(s);
    teardown();
    return crashed;
}

static void test_crash_during_format(void)
{
    u64 k = 1;
    for (;; k++) {
        const bool crashed = crash_format_once(k, FAULT_DROP_UNSYNCED, k);
        for (u32 i = 0; i < 2 * t_scale(); i++) {
            T_EQ(crash_format_once(k, FAULT_RANDOM_UNSYNCED, k * 1000u + i), crashed);
        }
        if (!crashed) {
            break;
        }
    }
    T_CHECK(k > 5);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_crash_during_checkpoints);
    T_RUN(test_crash_during_format);
    return t_done();
}
