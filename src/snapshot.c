#include "frames.h"
#include "ptable.h"
#include "store.h"
#include "util.h"

#include "pagestore/pagestore.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

/*
 * Snapshots read the newest committed version at or below their starting
 * LSN. The retire queue keeps replaced versions while a reader may need
 * them. In this chain a snapshot at LSN 15 sees the old data. At LSN 19,
 * the free marker says the page is gone.
 *
 *     head -> free at 19 -> data at 12
 *
 * Cut-off sets dead before unregistering a snapshot. The reader checks
 * it again under the latch before taking a pin, and existing pins keep
 * their versions alive.
 * Close releases pins before taking reg_mu to process the retire queue.
 */

/* One entry per pinned page ID, even when the caller reads it repeatedly. */
typedef struct ps_snap_slot {
    ps_page_id id;
    ps_vnode *v;
} ps_snap_slot;

struct ps_snapshot {
    ps_store *s;
    ps_snapref ref;
    u32 npins;
    u32 cap; /* cfg.snapshot_pin_cap */
    ps_snap_slot slots[]; /* npins used, unordered */
};

static u64 snap_size(u32 cap)
{
    return (u64) sizeof(ps_snapshot) + (u64) cap * sizeof(ps_snap_slot);
}

/* ---- retire queue -------------------------------------------------------- */

void ps_retire_push(ps_store *s, ps_vnode *v, ps_lsn r)
{
    PS_ASSERT((v->flags & (PS_VF_RETIRED | PS_VF_FREE_WHEN_UNPINNED)) == 0);
    PS_ASSERT(v != v->entry->head);
    PS_ASSERT(s->retire_tail == NULL || ps_lsn_le(s->retire_tail->retire_r, r));
    v->flags |= PS_VF_RETIRED;
    if (v->frame != PS_NO_FRAME) {
        atomic_fetch_add_explicit(&s->frames_retired, 1, memory_order_relaxed);
    }
    v->retire_r = r;
    v->retire_next = NULL;
    if (s->retire_tail != NULL) {
        s->retire_tail->retire_next = v;
    } else {
        s->retire_head = v;
    }
    s->retire_tail = v;
}

void ps_retire_process(ps_store *s)
{
    const ps_lsn min = ps_reg_min(s);
    while (s->retire_head != NULL && ps_lsn_le(s->retire_head->retire_r, min)) {
        ps_vnode *v = s->retire_head;
        s->retire_head = v->retire_next;
        if (s->retire_head == NULL) {
            s->retire_tail = NULL;
        }
        v->retire_next = NULL;
        /* The queued node keeps its entry alive before we take the latch. */
        ps_entry *e = v->entry;
        const ps_page_id id = e->id;
        ps_latch(s->pt, id);
        PS_ASSERT((v->flags & PS_VF_RETIRED) != 0);
        v->flags &= (u8) ~PS_VF_RETIRED;
        if (v->frame != PS_NO_FRAME) {
            atomic_fetch_sub_explicit(&s->frames_retired, 1, memory_order_relaxed);
        }
        ps_node_free(s, e, v);
        ps_unlatch(s->pt, id);
    }
}

/* ---- cut-off ------------------------------------------------------------- */

/*
 * Hold reg_mu. A queued version can free a frame only when the oldest
 * reader reaches its replacement LSN and the version has no pins.
 */
static bool retire_frees_frame(ps_store *s, ps_lsn min)
{
    for (ps_vnode *v = s->retire_head; v != NULL && ps_lsn_le(v->retire_r, min);
         v = v->retire_next) {
        const ps_page_id id = v->entry->id;
        ps_latch(s->pt, id);
        const bool framed = v->frame != PS_NO_FRAME && v->pins == 0;
        ps_unlatch(s->pt, id);
        if (framed) {
            return true;
        }
    }
    return false;
}

/* Enter with reg_mu held. Release it before logging. */
static void cut_off_unlock(ps_store *s, ps_snapref *r)
{
    const ps_lsn lsn = r->lsn;
    atomic_store_explicit(&r->dead, true, memory_order_release);
    ps_reg_remove(s, r);
    atomic_fetch_add_explicit(&s->snapshots_cut_off, 1, memory_order_relaxed);
    ps_retire_process(s);
    pthread_mutex_unlock(&s->reg_mu);
    PS_LOG_INFO(
        &s->logger,
        "snapshot at LSN %llu cut off to free memory",
        (unsigned long long) lsn.raw);
}

bool ps_snap_cut_off_oldest(ps_store *s)
{
    pthread_mutex_lock(&s->reg_mu);
    ps_snapref *r = s->reg_head;
    while (r != NULL && r->is_ckpt) {
        r = r->next;
    }
    if (r == NULL) {
        pthread_mutex_unlock(&s->reg_mu);
        return false;
    }
    cut_off_unlock(s, r);
    return true;
}

bool ps_snap_cut_off_for_frames(ps_store *s)
{
    pthread_mutex_lock(&s->reg_mu);
    ps_snapref *r = s->reg_head;
    /*
     * The oldest reader can advance at most to the checkpoint's LSN. Cut off
     * one snapshot at a time; the caller retries the frame request in between.
     */
    ps_lsn reach = PS_LSN_MAX;
    for (const ps_snapref *q = r; q != NULL; q = q->next) {
        if (q->is_ckpt) {
            reach = q->lsn;
            break;
        }
    }
    if (r == NULL || r->is_ckpt || !retire_frees_frame(s, reach)) {
        pthread_mutex_unlock(&s->reg_mu);
        return false;
    }
    cut_off_unlock(s, r);
    return true;
}

/* ---- snapshots ----------------------------------------------------------- */

int ps_snapshot_open(ps_store *s, ps_snapshot **out)
{
    if (atomic_load_explicit(&s->failed, memory_order_acquire)) {
        return EIO;
    }
    const u32 cap = s->cfg.snapshot_pin_cap;
    const u64 size = snap_size(cap);
    if (size > SIZE_MAX) {
        return ENOMEM;
    }
    ps_snapshot *sn = malloc((usize) size);
    if (sn == NULL) {
        return ENOMEM;
    }
    sn->s = s;
    sn->npins = 0;
    sn->cap = cap;
    sn->ref.is_ckpt = false;
    sn->ref.prev = NULL;
    sn->ref.next = NULL;
    atomic_init(&sn->ref.dead, false);

    /* Capture and register together so commit keeps the versions we need. */
    pthread_mutex_lock(&s->reg_mu);
    sn->ref.lsn = ps_lsn_from(atomic_load_explicit(&s->committed_lsn, memory_order_relaxed));
    ps_reg_add(s, &sn->ref);
    pthread_mutex_unlock(&s->reg_mu);

    atomic_fetch_add_explicit(&s->snapshots_open, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->snap_bytes, size, memory_order_relaxed);
    *out = sn;
    return 0;
}

ps_lsn ps_snapshot_lsn(const ps_snapshot *sn)
{
    return sn->ref.lsn;
}

static bool is_dead(const ps_snapshot *sn)
{
    return atomic_load_explicit(&sn->ref.dead, memory_order_acquire);
}

static ps_snap_slot *find_slot(ps_snapshot *sn, ps_page_id id)
{
    for (u32 i = 0; i < sn->npins; i++) {
        if (ps_page_id_eq(sn->slots[i].id, id)) {
            return &sn->slots[i];
        }
    }
    return NULL;
}

/* Each snapshot uses one unit even when another snapshot pins the same frame. */
static bool budget_take(ps_store *s)
{
    u32 cur = atomic_load_explicit(&s->snap_pins, memory_order_relaxed);
    while (cur < s->max_pinned) {
        if (atomic_compare_exchange_weak_explicit(
                &s->snap_pins,
                &cur,
                cur + 1,
                memory_order_relaxed,
                memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

static void budget_give(ps_store *s, u32 n)
{
    const u32 was = atomic_fetch_sub_explicit(&s->snap_pins, n, memory_order_relaxed);
    PS_ASSERT(was >= n);
    PS_UNUSED(was);
}

/* Report misuse to the caller so it can log outside the latch. */
typedef enum snap_misuse {
    MISUSE_NONE,
    MISUSE_FREED,
    MISUSE_NOT_ALLOCATED
} snap_misuse;

/* Find and pin the visible version, loading it if needed. Returns unlatched. */
static int pin_visible(ps_snapshot *sn, ps_page_id id, ps_vnode **out, snap_misuse *why)
{
    ps_store *s = sn->s;
    const ps_lsn S = sn->ref.lsn;
    ps_latch(s->pt, id);
    for (;;) {
        ps_entry *e = ps_entry_find(s->pt, id);
        if (e != NULL && (e->flags & PS_E_LOADING) != 0) {
            e->waiters++;
            ps_latch_wait(s->pt, id);
            e->waiters--;
            ps_entry_maybe_release(s->pt, e);
            continue;
        }
        if (is_dead(sn)) {
            ps_unlatch(s->pt, id);
            return ESTALE;
        }
        ps_vnode *v = e != NULL ? ps_chain_visible(e, S) : NULL;
        if (v == NULL && e != NULL && e->head != NULL) {
            ps_unlatch(s->pt, id);
            *why = MISUSE_NOT_ALLOCATED;
            return EINVAL;
        }
        if (v != NULL && v->kind == PS_V_FREE) {
            ps_unlatch(s->pt, id);
            *why = MISUSE_FREED;
            return EINVAL;
        }
        if (v != NULL && v->frame != PS_NO_FRAME) {
            if (ps_lsn_gt(ps_node_page(s, v)->lsn, S)) {
                ps_unlatch(s->pt, id);
                *why = MISUSE_NOT_ALLOCATED;
                return EINVAL;
            }
            /* Nothing a live snapshot can see is waiting to be freed. */
            PS_ASSERT((v->flags & PS_VF_FREE_WHEN_UNPINNED) == 0);
            ps_node_pin(s, v);
            ps_update_evictable(s, e);
            ps_frames_touch(s->frames, v->frame);
            atomic_fetch_add_explicit(&s->cache_hits, 1, memory_order_relaxed);
            ps_unlatch(s->pt, id);
            *out = v;
            return 0;
        }

        int rv = 0;
        if (e == NULL) {
            rv = ps_entry_get(s->pt, id, &e);
        }
        ps_vnode *got = NULL;
        if (rv == 0) {
            rv = ps_page_in(s, e, v, false, &got);
        }
        if (rv != 0) {
            ps_unlatch(s->pt, id); /* e may be gone */
            return rv;
        }
        /* The latch was dropped for the I/O: a cut-off may have happened meanwhile. */
        if (is_dead(sn)) {
            ps_node_unpin(s, e, got);
            ps_unlatch(s->pt, id);
            return ESTALE;
        }
        if (ps_lsn_gt(ps_node_page(s, got)->lsn, S)) {
            /* The disk copy's LSN may be unknown until page-in, for example after ps_free. */
            ps_node_unpin(s, e, got);
            ps_unlatch(s->pt, id);
            *why = MISUSE_NOT_ALLOCATED;
            return EINVAL;
        }
        ps_unlatch(s->pt, id);
        *out = got;
        return 0;
    }
}

int ps_snapshot_read(ps_snapshot *sn, ps_page_id id, const ps_page **page)
{
    ps_store *s = sn->s;
    if (is_dead(sn)) {
        return ESTALE;
    }
    if (id.raw == UINT32_MAX || ps_is_bitmap_id(s, id)) {
        PS_LOG_ERROR(&s->logger, "ps_snapshot_read: page %u is not a user page", id.raw);
        return EINVAL;
    }
    const ps_snap_slot *held = find_slot(sn, id);
    if (held != NULL) {
        *page = ps_node_page(s, held->v);
        return 0;
    }
    if (sn->npins >= sn->cap || !budget_take(s)) {
        return ENOBUFS;
    }
    ps_vnode *v = NULL;
    snap_misuse why = MISUSE_NONE;
    const int rv = pin_visible(sn, id, &v, &why);
    if (rv != 0) {
        budget_give(s, 1);
        if (why == MISUSE_FREED) {
            PS_LOG_ERROR(
                &s->logger,
                "ps_snapshot_read: page %u is freed at LSN %llu",
                id.raw,
                (unsigned long long) sn->ref.lsn.raw);
        } else if (why == MISUSE_NOT_ALLOCATED) {
            PS_LOG_ERROR(
                &s->logger,
                "ps_snapshot_read: page %u is not allocated at LSN %llu",
                id.raw,
                (unsigned long long) sn->ref.lsn.raw);
        }
        return rv;
    }
    sn->slots[sn->npins].id = id;
    sn->slots[sn->npins].v = v;
    sn->npins++;
    *page = ps_node_page(s, v);
    return 0;
}

/* Drops the snapshot's pin on v (a version of id). Latches id only. */
static void unpin_node(ps_store *s, ps_page_id id, ps_vnode *v)
{
    ps_latch(s->pt, id);
    ps_entry *e = v->entry; /* pinned, so still in e's chain */
    PS_ASSERT(e != NULL && ps_page_id_eq(e->id, id));
    ps_node_unpin(s, e, v);
    ps_unlatch(s->pt, id);
}

void ps_snapshot_unpin(ps_snapshot *sn, ps_page_id id)
{
    ps_snap_slot *slot = find_slot(sn, id);
    if (slot == NULL) {
        PS_ASSERT(!"ps_snapshot_unpin: page is not pinned by this snapshot");
        return;
    }
    ps_vnode *v = slot->v;
    *slot = sn->slots[--sn->npins];
    unpin_node(sn->s, id, v);
    budget_give(sn->s, 1);
}

void ps_snapshot_close(ps_snapshot *sn)
{
    if (sn == NULL) {
        return;
    }
    ps_store *s = sn->s;
    for (u32 i = 0; i < sn->npins; i++) {
        unpin_node(s, sn->slots[i].id, sn->slots[i].v);
    }
    if (sn->npins > 0) {
        budget_give(s, sn->npins);
    }
    pthread_mutex_lock(&s->reg_mu);
    if (!atomic_load_explicit(&sn->ref.dead, memory_order_relaxed)) {
        ps_reg_remove(s, &sn->ref); /* unless a cut-off already did */
    }
    ps_retire_process(s);
    pthread_mutex_unlock(&s->reg_mu);
    atomic_fetch_sub_explicit(&s->snapshots_open, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&s->snap_bytes, snap_size(sn->cap), memory_order_relaxed);
    free(sn);
}
