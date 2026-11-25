#include "allocator.h"
#include "mem_backend.h"
#include "ptable.h"
#include "rng.h"
#include "store.h"
#include "test.h"

#include "pagestore/backend.h"
#include "pagestore/bytes.h"
#include "pagestore/pagestore.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PS 4096u
#define HDR PS_PAGE_HEADER_SIZE

/*
 * The operation limit is 11 frames: the 16-frame cache less four snapshot pins
 * and the resident bitmap page.
 */
#define FRAMES 16u
#define LIMIT 11u

static mem_backend *mb;
static ps_store *s;
static u64 next_lsn;

static void open_store(u32 frames)
{
    mb = mb_create();
    T_OK(mb_open(mb, frames, PS_CREATE, &s));
    next_lsn = 1;
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

static void begin(void)
{
    T_OK(ps_op_begin(s, ps_lsn_from(next_lsn++)));
}

/* Page contents: id and tag after the header, then a byte pattern from both. */
static void stamp(ps_page *p, ps_page_id id, u32 tag)
{
    u8 *b = (u8 *) p;
    ps_put_le32(b + HDR, id.raw);
    ps_put_le32(b + HDR + 4, tag);
    memset(b + HDR + 8, (int) ((id.raw * 31u + tag) & 0xFFu), PS - HDR - 8);
}

static bool stamped(const ps_page *p, ps_page_id id, u32 tag)
{
    const u8 *b = (const u8 *) p;
    const u32 i = ps_get_le32(b + HDR);
    const u32 t = ps_get_le32(b + HDR + 4);
    if (i != id.raw || t != tag || !ps_page_id_eq(p->id, id) || p->checksum != 0) {
        return false;
    }
    const u8 want = (u8) ((id.raw * 31u + tag) & 0xFFu);
    for (u32 k = HDR + 8; k < PS; k++) {
        if (b[k] != want) {
            return false;
        }
    }
    return true;
}

static ps_page_id alloc_stamped(u32 tag)
{
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    stamp(p, id, tag);
    return id;
}

static void write_stamped(ps_page_id id, u32 tag)
{
    ps_page *p = NULL;
    T_OK(ps_write(s, id, &p));
    stamp(p, id, tag);
}

/* Checkpoint each batch so it can be evicted while the fixture grows. */
static void make_pages(u32 n, u32 per)
{
    u32 done = 0;
    while (done < n) {
        begin();
        for (u32 i = 0; i < per && done < n; i++) {
            T_EQ(alloc_stamped(1).raw, done + 1);
            done++;
        }
        ps_op_commit(s);
        T_OK(ps_checkpoint(s));
    }
    T_EQ(stats().frames_dirty, 0);
}

static ps_snapshot *snap_open(void)
{
    ps_snapshot *sn = NULL;
    T_OK(ps_snapshot_open(s, &sn));
    return sn;
}

static const ps_page *snap_get(ps_snapshot *sn, ps_page_id id)
{
    const ps_page *p = NULL;
    T_OK(ps_snapshot_read(sn, id, &p));
    T_CHECK(p != NULL);
    return p;
}

/* The version below the head must exist. It may have no frame. */
static u32 under_head_frame(ps_page_id id)
{
    ps_latch(s->pt, id);
    const ps_entry *e = ps_entry_find(s->pt, id);
    T_CHECK(e != NULL && e->head != NULL && e->head->older != NULL);
    const u32 f = e->head->older->frame;
    ps_unlatch(s->pt, id);
    return f;
}

/* ---- tests ---- */

static void test_op_pressure(void)
{
    open_store(FRAMES);
    ps_pressure p = pressure();
    T_EQ(p.op_frames, 0);
    T_EQ(p.op_limit, LIMIT);
    T_EQ(p.total_frames, FRAMES);
    T_EQ(p.dirty_frames, 0);

    begin();
    for (u32 i = 0; i < 3; i++) {
        alloc_stamped(1);
    }
    p = pressure();
    /* The operation holds three allocated pages and group 0's bitmap spare. */
    T_EQ(p.op_frames, 4);
    T_EQ(p.op_limit, LIMIT);
    T_EQ(p.total_frames, FRAMES);
    T_EQ(p.dirty_frames, 0);
    ps_op_commit(s);
    p = pressure();
    T_EQ(p.op_frames, 0);
    T_EQ(p.dirty_frames, 4); /* three pages and the bitmap page */
    T_OK(ps_checkpoint(s));
    T_EQ(pressure().dirty_frames, 0);

    /* A copy also holds the committed version under it, unless a ps_read pin counts it. */
    begin();
    write_stamped(ps_page_id_from(1), 2);
    T_EQ(pressure().op_frames, 2);
    const ps_page *rp = NULL;
    T_OK(ps_read(s, ps_page_id_from(2), &rp));
    T_EQ(pressure().op_frames, 3);
    write_stamped(ps_page_id_from(2), 2);
    T_EQ(pressure().op_frames, 4);
    T_OK(ps_read(s, ps_page_id_from(1), &rp)); /* the operation's own copy */
    T_OK(ps_read(s, ps_page_id_from(2), &rp));
    write_stamped(ps_page_id_from(1), 3);
    T_EQ(pressure().op_frames, 4);
    ps_op_abort(s);
    T_EQ(pressure().op_frames, 0);
    T_EQ(stats().frames_evictable, 3); /* the bases are evictable heads again */
    close_store();
}

static void test_op_too_big(void)
{
    open_store(FRAMES);
    const struct ps_stats st0 = stats();
    begin();
    for (u32 i = 0; i < LIMIT - 1; i++) {
        alloc_stamped(1);
    }
    T_EQ(pressure().op_frames, LIMIT);
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_ERR(ps_alloc(s, &id, &p), ENOBUFS);
    T_ERR(ps_alloc(s, &id, &p), ENOBUFS);
    /* At the limit, the operation's own pages still work. */
    T_OK(ps_write(s, ps_page_id_from(1), &p));
    const ps_page *rp = NULL;
    T_OK(ps_read(s, ps_page_id_from(2), &rp));
    T_EQ(pressure().op_frames, LIMIT);
    /* The operation limit should fail before eviction or snapshot cut-off. */
    T_EQ(stats().frames_free, st0.frames_free - LIMIT);
    ps_op_abort(s);
    T_EQ(stats().frames_free, st0.frames_free);

    make_pages(12, LIMIT - 1); /* the store is usable */

    /* After ten read pins, copying an unread page needs two more frames. */
    begin();
    for (u32 i = 1; i <= LIMIT - 1; i++) {
        T_OK(ps_read(s, ps_page_id_from(i), &rp));
    }
    T_ERR(ps_write(s, ps_page_id_from(LIMIT), &p), ENOBUFS);
    T_OK(ps_write(s, ps_page_id_from(1), &p)); /* pinned by the operation already: just the copy */
    T_EQ(pressure().op_frames, LIMIT);
    T_ERR(ps_read(s, ps_page_id_from(LIMIT), &rp), ENOBUFS);
    T_ERR(ps_write(s, ps_page_id_from(2), &p), ENOBUFS);
    T_OK(ps_read(s, ps_page_id_from(2), &rp)); /* pinned already */
    ps_op_abort(s);
    T_EQ(stats().frames_pinned, 0);
    close_store();
}

static void test_eviction_reuses_clean(void)
{
    open_store(FRAMES);
    make_pages(100, LIMIT - 1);
    const struct ps_stats st0 = stats();
    for (u32 i = 0; i < 25; i++) {
        begin();
        for (u32 k = 1; k <= 4; k++) {
            const ps_page *p = NULL;
            T_OK(ps_read(s, ps_page_id_from(i * 4 + k), &p));
            T_CHECK(stamped(p, ps_page_id_from(i * 4 + k), 1));
        }
        ps_op_commit(s);
    }
    const struct ps_stats st = stats();
    T_CHECK(st.cache_misses - st0.cache_misses >= 84);
    T_EQ(st.snapshots_cut_off, 0);
    T_EQ(st.frames_pinned, 0);
    close_store();
}

static void test_dirty_not_evicted_eagain(void)
{
    open_store(FRAMES);
    int rv = 0;
    u32 ops = 0;
    for (; ops < 10 && rv == 0; ops++) {
        begin();
        for (u32 i = 0; i < 5 && rv == 0; i++) {
            ps_page_id id = PS_PAGE_ID_NONE;
            ps_page *p = NULL;
            rv = ps_alloc(s, &id, &p);
            if (rv == 0) {
                stamp(p, id, 1);
            }
        }
        if (rv == 0) {
            ps_op_commit(s);
        }
    }
    T_ERR(rv, EAGAIN);
    T_EQ(ops, 3); /* 15 frames for pages: the third operation runs out of clean frames */
    ps_op_abort(s);
    T_EQ(stats().snapshots_cut_off, 0);
    T_OK(ps_checkpoint(s));
    begin();
    for (u32 i = 0; i < 5; i++) {
        alloc_stamped(1);
    }
    ps_op_commit(s);
    T_EQ(stats().allocated_pages, 15);
    ps_snapshot *sn = snap_open();
    for (u32 i = 1; i <= 15; i++) {
        const ps_page_id id = ps_page_id_from(i);
        T_CHECK(stamped(snap_get(sn, id), id, 1));
        ps_snapshot_unpin(sn, id);
    }
    ps_snapshot_close(sn);
    close_store();
}

static void test_cutoff_under_pressure(void)
{
    open_store(FRAMES);
    make_pages(10, 10);
    ps_snapshot *sn = snap_open();
    const ps_page *held = snap_get(sn, ps_page_id_from(1));
    T_CHECK(stamped(held, ps_page_id_from(1), 1));

    u32 rounds = 0;
    for (; rounds < 10 && stats().snapshots_cut_off == 0; rounds++) {
        begin();
        for (u32 k = 1; k <= 5; k++) {
            write_stamped(ps_page_id_from((rounds % 2) * 5 + k), rounds + 2);
        }
        ps_op_commit(s);
        T_OK(ps_checkpoint(s));
    }
    T_EQ(stats().snapshots_cut_off, 1);
    T_EQ(rounds, 3); /* the third round needs the retired frames */
    const ps_page *p = NULL;
    T_ERR(ps_snapshot_read(sn, ps_page_id_from(2), &p), ESTALE);
    T_CHECK(stamped(held, ps_page_id_from(1), 1)); /* still pinned, still the old bytes */
    ps_snapshot_close(sn);
    struct ps_stats st = stats();
    T_EQ(st.frames_retired, 0);
    T_EQ(st.frames_pinned, 0);
    T_EQ(st.snapshots_open, 0);

    ps_snapshot *s2 = snap_open();
    for (u32 i = 1; i <= 10; i++) {
        const ps_page_id id = ps_page_id_from(i);
        const u32 tag = i <= 5 ? 4 : 3;
        T_CHECK(stamped(snap_get(s2, id), id, tag));
        ps_snapshot_unpin(s2, id);
    }
    ps_snapshot_close(s2);
    close_store();
}

static void test_reader_never_cuts_off(void)
{
    open_store(FRAMES);
    make_pages(10, 10);
    ps_snapshot *sn = snap_open();
    /* Three retired versions for sn and three dirty copies. */
    begin();
    for (u32 i = 1; i <= 3; i++) {
        const ps_page_id id = ps_page_id_from(i);
        write_stamped(id, 2);
    }
    ps_op_commit(s);
    struct ps_stats st = stats();
    T_EQ(st.frames_retired, 3);
    T_EQ(st.frames_free, 2);
    /* An operation takes every other frame: 2 free, 7 evicted. */
    begin();
    for (u32 i = 0; i < 8; i++) {
        alloc_stamped(1);
    }
    st = stats();
    T_EQ(st.frames_free, 0);
    T_EQ(st.frames_evictable, 0);
    T_EQ(st.snapshots_cut_off, 0);

    const ps_page *p = NULL;
    T_ERR(ps_snapshot_read(sn, ps_page_id_from(4), &p), EAGAIN); /* evicted; dirty frames exist */
    T_EQ(stats().snapshots_cut_off, 0);
    /* resident retired version, still alive */
    T_CHECK(stamped(snap_get(sn, ps_page_id_from(1)), ps_page_id_from(1), 1));
    ps_snapshot_unpin(sn, ps_page_id_from(1));

    alloc_stamped(1);
    T_EQ(stats().snapshots_cut_off, 1);
    T_ERR(ps_snapshot_read(sn, ps_page_id_from(1), &p), ESTALE);
    ps_op_abort(s);
    ps_snapshot_close(sn);
    T_EQ(stats().frames_retired, 0);
    close_store();
}

/* Everything a checkpoint could clean is already clean. */
static void test_reader_enobufs(void)
{
    open_store(FRAMES);
    make_pages(4, 4);
    ps_snapshot *sn = snap_open();
    for (u32 i = 1; i <= 3; i++) {
        const ps_page_id id = ps_page_id_from(i);
        snap_get(sn, id);
    }
    begin();
    write_stamped(ps_page_id_from(4), 2);
    ps_op_commit(s);
    T_OK(ps_checkpoint(s)); /* page 4's old version stays retired for sn */
    begin();
    for (u32 i = 0; i < LIMIT - 1; i++) {
        alloc_stamped(1);
    }
    struct ps_stats st = stats();
    T_EQ(st.frames_free, 0);
    T_EQ(st.frames_evictable, 0);
    T_EQ(st.frames_dirty, 0);
    T_EQ(st.frames_retired, 1);

    ps_snapshot *s2 = snap_open();
    const ps_page *p = NULL;
    T_ERR(ps_snapshot_read(s2, ps_page_id_from(4), &p), ENOBUFS); /* page 4's head was evicted */
    T_CHECK(stamped(snap_get(sn, ps_page_id_from(4)), ps_page_id_from(4), 1));
    T_EQ(stats().snapshots_cut_off, 0);
    ps_snapshot_close(s2);
    ps_op_abort(s);
    ps_snapshot_unpin(sn, ps_page_id_from(4));
    s2 = snap_open();
    T_CHECK(stamped(snap_get(s2, ps_page_id_from(4)), ps_page_id_from(4), 2)); /* frames again */
    ps_snapshot_close(s2);
    ps_snapshot_close(sn);
    close_store();
}

static void test_reload_after_evict(void)
{
    open_store(FRAMES);
    make_pages(30, LIMIT - 1);
    /* Check both forced eviction and reuse by CLOCK. */
    T_CHECK(ps_test_evict(s, ps_page_id_from(30))); /* written last, so resident */
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(30)));
    T_CHECK(!ps_test_evict(s, ps_page_id_from(30)));
    const struct ps_stats st0 = stats();
    ps_snapshot *sn = snap_open();
    const ps_page *p = snap_get(sn, ps_page_id_from(30));
    T_CHECK(stamped(p, ps_page_id_from(30), 1));
    T_EQ(p->checksum, 0);
    T_EQ(p->lsn.raw, ((const ps_page *) mb_page(mb, ps_page_id_from(30)))->lsn.raw);
    T_MEMEQ((const u8 *) p + HDR, mb_page(mb, ps_page_id_from(30)) + HDR, PS - HDR);
    T_EQ(stats().cache_misses, st0.cache_misses + 1);
    ps_snapshot_unpin(sn, ps_page_id_from(30));
    for (u32 pass = 0; pass < 2; pass++) {
        for (u32 i = 1; i <= 30; i++) {
            const ps_page_id id = ps_page_id_from(i);
            T_CHECK(stamped(snap_get(sn, id), id, 1));
            ps_snapshot_unpin(sn, id);
        }
    }
    T_CHECK(stats().cache_misses >= st0.cache_misses + 1 + 2 * (30 - (FRAMES - 1)));
    ps_snapshot_close(sn);
    close_store();
}

/* Keep a node for the disk version above its retired versions. */
static void test_evict_head_keeps_frameless(void)
{
    open_store(FRAMES);
    make_pages(1, 1);
    ps_snapshot *sn = snap_open();
    begin();
    write_stamped(ps_page_id_from(1), 2);
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 2);
    T_CHECK(ps_test_evict(s, ps_page_id_from(1)));
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 2);
    T_EQ(stats().frames_retired, 1);
    /* the retired version, resident */
    T_CHECK(stamped(snap_get(sn, ps_page_id_from(1)), ps_page_id_from(1), 1));
    const u64 misses = stats().cache_misses;
    begin();
    const ps_page *p = NULL;
    T_OK(ps_read(s, ps_page_id_from(1), &p)); /* the head is reloaded */
    T_CHECK(stamped(p, ps_page_id_from(1), 2));
    T_EQ(stats().cache_misses, misses + 1);
    ps_op_commit(s);
    ps_snapshot_close(sn);
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 1);
    T_EQ(stats().frames_retired, 0);
    close_store();
}

/*
 * A frameless head left alone is dropped: after eviction,
 * once the retired versions below it are freed, and after an aborted free.
 */
static void test_lone_frameless_dropped(void)
{
    open_store(FRAMES);
    make_pages(2, 2);
    ps_snapshot *sn = snap_open();
    begin();
    write_stamped(ps_page_id_from(1), 2);
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    T_CHECK(ps_test_evict(s, ps_page_id_from(1)));
    T_EQ(ps_test_chain_len(s, ps_page_id_from(1)), 2);
    ps_snapshot_close(sn); /* frees the retired version below the frameless head */
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(1)));

    begin();
    T_OK(ps_free(s, ps_page_id_from(2))); /* clean: its frame is released at once */
    ps_op_abort(s);
    T_CHECK(!ps_test_has_entry(s, ps_page_id_from(2)));

    begin();
    const ps_page *p = NULL;
    T_OK(ps_read(s, ps_page_id_from(1), &p));
    T_CHECK(stamped(p, ps_page_id_from(1), 2));
    T_OK(ps_read(s, ps_page_id_from(2), &p));
    T_CHECK(stamped(p, ps_page_id_from(2), 1));
    ps_op_commit(s);
    close_store();
}

static void *bg_checkpoint(void *arg)
{
    int *rv = arg;
    *rv = ps_checkpoint(s);
    return NULL;
}

static bool ckpt_running(void)
{
    pthread_mutex_lock(&s->reg_mu);
    const bool r = s->ckpt_running;
    pthread_mutex_unlock(&s->reg_mu);
    return r;
}

/*
 * Cutting off snapshots cannot free versions retired for a running checkpoint,
 * so the writer gets EAGAIN and no snapshot is cut off for nothing.
 */
static void test_no_cutoff_behind_checkpoint(void)
{
    open_store(FRAMES);
    make_pages(10, 10);
    begin();
    write_stamped(ps_page_id_from(10), 2);
    ps_op_commit(s);
    mb_block_writes(mb, true);
    int crv = -1;
    pthread_t t;
    T_OK(pthread_create(&t, NULL, bg_checkpoint, &crv));
    while (!ckpt_running()) {
        t_sleep_ms(1);
    }
    ps_snapshot *s1 = snap_open();
    ps_snapshot *s2 = snap_open();
    int rv = 0;
    for (u32 round = 0; round < 10 && rv == 0; round++) {
        begin();
        for (u32 k = 1; k <= 5 && rv == 0; k++) {
            ps_page *p = NULL;
            rv = ps_write(s, ps_page_id_from(k + (round % 2) * 4), &p);
            if (rv == 0) {
                stamp(p, ps_page_id_from(k + (round % 2) * 4), (u32) round + 3);
            }
        }
        if (rv == 0) {
            ps_op_commit(s);
        } else {
            ps_op_abort(s);
        }
    }
    T_ERR(rv, EAGAIN);
    struct ps_stats st = stats();
    T_EQ(st.snapshots_cut_off, 0);
    T_CHECK(st.frames_retired > 0);
    const ps_page *p = NULL;
    T_OK(ps_snapshot_read(s1, ps_page_id_from(10), &p)); /* still alive */
    T_CHECK(stamped(p, ps_page_id_from(10), 2));
    mb_block_writes(mb, false);
    T_OK(pthread_join(t, NULL));
    T_OK(crv);
    ps_snapshot_close(s1);
    ps_snapshot_close(s2);
    T_EQ(stats().frames_retired, 0);
    close_store();
}

static void test_under_inop_not_evicted(void)
{
    open_store(FRAMES);
    make_pages(30, LIMIT - 1);
    ps_snapshot *sn = snap_open();
    begin();
    write_stamped(ps_page_id_from(1), 2);
    const u32 base = under_head_frame(ps_page_id_from(1));
    T_NE(base, PS_NO_FRAME);
    T_CHECK(!ps_test_evict(s, ps_page_id_from(1)));
    /* Another reader cycles every other page through the cache. */
    ps_snapshot *r = snap_open();
    const u64 misses0 = stats().cache_misses;
    for (u32 pass = 0; pass < 3; pass++) {
        for (u32 i = 2; i <= 30; i++) {
            const ps_page_id id = ps_page_id_from(i);
            T_CHECK(stamped(snap_get(r, id), id, 1));
            ps_snapshot_unpin(r, id);
        }
        T_EQ(under_head_frame(ps_page_id_from(1)), base);
    }
    T_CHECK(stats().cache_misses - misses0 >= 3 * 15);
    ps_snapshot_close(r);
    ps_op_commit(s);
    const u64 misses = stats().cache_misses;
    T_CHECK(stamped(snap_get(sn, ps_page_id_from(1)), ps_page_id_from(1), 1));
    T_EQ(stats().cache_misses, misses); /* it never left */
    ps_snapshot_close(sn);
    close_store();
}

/* A clean base has a disk copy, so the free marker can give back its frame. */
static void test_free_releases_clean_base(void)
{
    open_store(FRAMES);
    make_pages(3, 3);
    ps_snapshot *sn = snap_open();
    const u32 free0 = stats().frames_free;
    begin();
    T_OK(ps_free(s, ps_page_id_from(1)));
    T_EQ(pressure().op_frames, 1); /* group 0's spare only */
    T_EQ(stats().frames_free, free0); /* -1 spare, +1 page 1's frame */
    T_EQ(under_head_frame(ps_page_id_from(1)), PS_NO_FRAME);
    /* A snapshot loads it; its last unpin drops the frame again. */
    T_CHECK(stamped(snap_get(sn, ps_page_id_from(1)), ps_page_id_from(1), 1));
    T_EQ(stats().frames_free, free0 - 1);
    ps_snapshot_unpin(sn, ps_page_id_from(1));
    T_EQ(stats().frames_free, free0);
    /* Free after write must return both the copy frame and the clean base frame. */
    write_stamped(ps_page_id_from(2), 2);
    T_EQ(pressure().op_frames, 3);
    T_OK(ps_free(s, ps_page_id_from(2)));
    T_EQ(pressure().op_frames, 1);
    T_EQ(under_head_frame(ps_page_id_from(2)), PS_NO_FRAME);
    /* Freed while a snapshot pins it: it keeps its frame until that unpin. */
    T_CHECK(stamped(snap_get(sn, ps_page_id_from(3)), ps_page_id_from(3), 1));
    T_OK(ps_free(s, ps_page_id_from(3)));
    T_EQ(pressure().op_frames, 1);
    T_NE(under_head_frame(ps_page_id_from(3)), PS_NO_FRAME);
    ps_snapshot_unpin(sn, ps_page_id_from(3));
    T_EQ(under_head_frame(ps_page_id_from(3)), PS_NO_FRAME);
    ps_op_commit(s);
    for (u32 i = 1; i <= 3; i++) {
        const ps_page_id id = ps_page_id_from(i);
        T_CHECK(stamped(snap_get(sn, id), id, 1)); /* from the data file */
    }
    ps_snapshot_close(sn);
    T_EQ(stats().frames_retired, 0);
    T_EQ(stats().allocated_pages, 0);

    begin();
    ps_page_id id = alloc_stamped(5);
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    begin();
    T_OK(ps_free(s, id));
    ps_op_abort(s);
    begin();
    const ps_page *p = NULL;
    T_OK(ps_read(s, id, &p));
    T_CHECK(stamped(p, id, 5));
    ps_op_commit(s);
    close_store();
}

static void test_chunked_job(void)
{
    open_store(FRAMES);
    u32 ops = 0;
    begin();
    for (u32 i = 1; i <= 1000; i++) {
        T_EQ(alloc_stamped(7).raw, i);
        const ps_pressure p = pressure();
        if (p.op_frames >= p.op_limit - 2) {
            ps_op_commit(s);
            T_OK(ps_checkpoint(s));
            begin();
            ops++;
        }
    }
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    T_CHECK(ops >= 1000 / LIMIT);
    T_EQ(stats().allocated_pages, 1000);
    ps_snapshot *sn = snap_open();
    for (u32 i = 1; i <= 1000; i++) {
        const ps_page_id id = ps_page_id_from(i);
        T_CHECK(stamped(snap_get(sn, id), id, 7));
        ps_snapshot_unpin(sn, id);
    }
    ps_snapshot_close(sn);
    close_store();
}

/* Bitmap pages take a frame each: open refuses a cache they leave too small. */
struct ps_logger_ctx {
    atomic_int too_small;
    atomic_int group; /* ps_alloc refused a new group */
};

static void log_fn(ps_logger_ctx *ctx, ps_log_level level, const char *fmt, va_list ap)
{
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    if (level == PS_LOG_LEVEL_ERROR && strstr(msg, "too small") != NULL) {
        atomic_fetch_add(&ctx->too_small, 1);
    }
    if (level == PS_LOG_LEVEL_ERROR && strstr(msg, "larger cache") != NULL) {
        atomic_fetch_add(&ctx->group, 1);
    }
}

static void test_open_cache_too_small(void)
{
    ps_logger_ctx lc;
    atomic_init(&lc.too_small, 0);
    atomic_init(&lc.group, 0);
    ps_config cfg = mb_config(FRAMES);
    cfg.logger = (ps_logger) {.max_level = PS_LOG_LEVEL_DEBUG, .ctx = &lc, .write = log_fn};
    ps_backend be;

    /* Reserving 12 pins and a bitmap leaves only three frames for the writer. */
    mb = mb_create();
    cfg.max_pinned_pct = 75;
    mb_backend(mb, &be);
    T_ERR(ps_open_backend(&be, &cfg, PS_CREATE, &s), EINVAL);
    T_EQ(atomic_load(&lc.too_small), 1);
    T_EQ(mb_ckpt_count(mb), 0); /* refused before formatting */
    /* The 68% share rounds down to ten pins, leaving five frames. */
    cfg.max_pinned_pct = 68;
    mb_backend(mb, &be);
    T_OK(ps_open_backend(&be, &cfg, PS_CREATE, &s));
    T_EQ(pressure().op_limit, 5);
    ps_close(s);
    mb_destroy(mb);

    /* An existing store with 12 groups: 4 + 12 + 4 > 16, refused before its bitmaps are read. */
    mb = mb_create();
    ps_backend_info info = {PS, 12, 1, ps_lsn_from(9)};
    mb_set_info(mb, info);
    cfg.max_pinned_pct = 25;
    mb_backend(mb, &be);
    T_ERR(ps_open_backend(&be, &cfg, 0, &s), EINVAL);
    T_EQ(atomic_load(&lc.too_small), 2);
    mb_destroy(mb);
    mb = NULL;
    s = NULL;
}

/* An existing store (durable 9) of ngroups full groups: only bitmap pages on disk. */
static void inject_full_groups(u32 ngroups)
{
    mb = mb_create();
    ps_backend_info info = {PS, ngroups, 1, ps_lsn_from(9)};
    mb_set_info(mb, info);
    u8 *buf = t_calloc(1, PS);
    for (u32 g = 0; g < ngroups; g++) {
        memset(buf, 0, PS);
        ps_page *p = (ps_page *) buf;
        p->magic = PS_BITMAP_MAGIC;
        p->id = ps_page_id_from(g * ps_group_size(PS));
        p->flags = PS_PAGE_FLAG_BITMAP;
        p->lsn = ps_lsn_from(9);
        memset(buf + HDR, 0xFF, PS - HDR);
        mb_seal_page(buf, PS);
        mb_put(mb, p->id, buf);
    }
    free(buf);
}

/*
 * A group the cache cannot hold is refused: 32 frames - 8 snapshot pins - 4
 * leave room for 20 bitmap pages, so with 20 full groups ps_alloc gets
 * ENOBUFS (logged) instead of creating a 21st.
 */
static void test_group_limit(void)
{
    ps_logger_ctx lc;
    atomic_init(&lc.too_small, 0);
    atomic_init(&lc.group, 0);
    ps_config cfg = mb_config(32);
    cfg.logger = (ps_logger) {.max_level = PS_LOG_LEVEL_DEBUG, .ctx = &lc, .write = log_fn};
    inject_full_groups(20);
    ps_backend be;
    mb_backend(mb, &be);
    T_OK(ps_open_backend(&be, &cfg, 0, &s));
    next_lsn = 10;
    T_EQ(stats().num_groups, 20);
    T_EQ(pressure().op_limit, 4);
    begin();
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_ERR(ps_alloc(s, &id, &p), ENOBUFS);
    T_EQ(atomic_load(&lc.group), 1);
    T_EQ(pressure().op_frames, 0);
    ps_op_abort(s);
    T_EQ(stats().num_groups, 20);
    T_EQ(stats().frames_free, 32 - 20);
    close_store();
}

/* ---- concurrent eviction ---- */

enum {
    CONC_PAGES = 48,
    CONC_READERS = 3,
    CONC_OPS = 300
};

typedef struct conc_reader {
    pthread_t t;
    u64 seed;
    u64 reads;
    u64 stale;
    u64 busy;
} conc_reader;

static atomic_bool conc_stop;
static u64 conc_base_lsn; /* make_pages' operations end here; their pages carry tag 1 */

/* A version's stamp: tag = the LSN that wrote it (1 for make_pages). */
static void *conc_reader_main(void *arg)
{
    conc_reader *r = arg;
    rng g;
    rng_seed(&g, r->seed);
    while (!atomic_load(&conc_stop)) {
        ps_snapshot *sn = NULL;
        T_OK(ps_snapshot_open(s, &sn));
        const ps_lsn S = ps_snapshot_lsn(sn);
        for (u32 k = 0; k < 3; k++) {
            const ps_page_id id = ps_page_id_from(1 + (u32) rng_below(&g, CONC_PAGES));
            const ps_page *p = NULL;
            const int rv = ps_snapshot_read(sn, id, &p);
            if (rv == ESTALE) {
                r->stale++;
                break;
            }
            if (rv == EAGAIN || rv == ENOBUFS) {
                r->busy++;
                continue;
            }
            T_OK(rv);
            T_CHECK(ps_lsn_le(p->lsn, S));
            T_CHECK(stamped(p, id, p->lsn.raw <= conc_base_lsn ? 1 : (u32) p->lsn.raw));
            r->reads++;
        }
        ps_snapshot_close(sn);
    }
    return NULL;
}

static void test_concurrent_eviction(void)
{
    open_store(FRAMES);
    make_pages(CONC_PAGES, LIMIT - 1);
    conc_base_lsn = next_lsn - 1;
    atomic_store(&conc_stop, false);
    conc_reader rd[CONC_READERS];
    for (u32 i = 0; i < CONC_READERS; i++) {
        memset(&rd[i], 0, sizeof(rd[i]));
        rd[i].seed = 1000 + i;
        T_OK(pthread_create(&rd[i].t, NULL, conc_reader_main, &rd[i]));
    }
    rng g;
    rng_seed(&g, 42);
    for (u32 n = 0; n < CONC_OPS * t_scale(); n++) {
        for (;;) {
            const u64 lsn = next_lsn++;
            T_OK(ps_op_begin(s, ps_lsn_from(lsn)));
            int rv = 0;
            for (u32 k = 0; k < 3 && rv == 0; k++) {
                const ps_page_id id = ps_page_id_from(1 + (u32) rng_below(&g, CONC_PAGES));
                ps_page *p = NULL;
                rv = ps_write(s, id, &p);
                if (rv == 0) {
                    stamp(p, id, (u32) lsn);
                }
            }
            if (rv == 0) {
                ps_op_commit(s);
                break;
            }
            T_CHECK(rv == EAGAIN);
            ps_op_abort(s);
            T_OK(ps_checkpoint(s));
        }
        if (n % 2 == 1) {
            T_OK(ps_checkpoint(s));
        }
    }
    atomic_store(&conc_stop, true);
    u64 reads = 0;
    for (u32 i = 0; i < CONC_READERS; i++) {
        T_OK(pthread_join(rd[i].t, NULL));
        reads += rd[i].reads;
    }
    T_CHECK(reads > 0);
    const struct ps_stats st = stats();
    T_EQ(st.frames_retired, 0);
    T_EQ(st.frames_pinned, 0);
    T_EQ(st.snapshots_open, 0);
    close_store();
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_op_pressure);
    T_RUN(test_op_too_big);
    T_RUN(test_eviction_reuses_clean);
    T_RUN(test_dirty_not_evicted_eagain);
    T_RUN(test_cutoff_under_pressure);
    T_RUN(test_reader_never_cuts_off);
    T_RUN(test_reader_enobufs);
    T_RUN(test_reload_after_evict);
    T_RUN(test_evict_head_keeps_frameless);
    T_RUN(test_lone_frameless_dropped);
    T_RUN(test_no_cutoff_behind_checkpoint);
    T_RUN(test_under_inop_not_evicted);
    T_RUN(test_free_releases_clean_base);
    T_RUN(test_chunked_job);
    T_RUN(test_open_cache_too_small);
    T_RUN(test_group_limit);
    T_RUN(test_concurrent_eviction);
    return t_done();
}
