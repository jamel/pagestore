#include "allocator.h"
#include "bitmap.h"
#include "frames.h"
#include "ptable.h"
#include "store.h"
#include "util.h"

#include "pagestore/pagestore.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/*
 * Single-writer operations. New page versions are private while IN_OP
 * is set. Allocation changes stay in the allocator until commit applies
 * them to the bitmaps. The operation reserves what commit needs to
 * publish both sets of changes under reg_mu.
 */

/* State passed to the allocator's bitmap callback. */
struct ps_bitmap_get_ctx {
    ps_store *s;
    bool no_room; /* the callback refused a spare under the operation limit */
};

/* ---- small helpers -------------------------------------------------------- */

/* On allocation failure the caller still owns buf and *cap stays valid. */
static void *grow(ps_store *s, void *buf, u32 *cap, u32 n, usize size)
{
    if (n <= *cap) {
        return buf;
    }
    u64 want = *cap == 0 ? 16 : (u64) *cap * 2;
    while (want < n) {
        want *= 2;
    }
    if (want > UINT32_MAX) {
        want = UINT32_MAX;
    }
    void *p = realloc(buf, (usize) want * size);
    if (p == NULL) {
        return NULL;
    }
    atomic_fetch_add_explicit(&s->op_bytes, (want - *cap) * size, memory_order_relaxed);
    *cap = (u32) want;
    return p;
}

static int reserve_pages(ps_store *s, u32 n)
{
    ps_entry **p = grow(s, s->op.pages, &s->op.pages_cap, n, sizeof(*p));
    if (p == NULL) {
        return ENOMEM;
    }
    s->op.pages = p;
    return 0;
}

static int reserve_pins(ps_store *s, u32 n)
{
    ps_vnode **p = grow(s, s->op.pins, &s->op.pins_cap, n, sizeof(*p));
    if (p == NULL) {
        return ENOMEM;
    }
    s->op.pins = p;
    return 0;
}

static int reserve_groups(ps_store *s, u32 n)
{
    ps_op_group *p = grow(s, s->op.groups, &s->op.groups_cap, n, sizeof(*p));
    if (p == NULL) {
        return ENOMEM;
    }
    s->op.groups = p;
    return 0;
}

/*
 * Wait out any page-in. The caller owns the page latch even if no entry
 * remains.
 */
static ps_entry *latch_find(ps_store *s, ps_page_id id)
{
    ps_latch(s->pt, id);
    for (;;) {
        ps_entry *e = ps_entry_find(s->pt, id);
        if (e == NULL || (e->flags & PS_E_LOADING) == 0) {
            return e;
        }
        e->waiters++;
        ps_latch_wait(s->pt, id);
        e->waiters--;
        ps_entry_maybe_release(s->pt, e);
    }
}

/* The waiter count keeps e alive while the condvar drops its latch. */
static void wait_loaded(ps_store *s, ps_entry *e)
{
    while ((e->flags & PS_E_LOADING) != 0) {
        e->waiters++;
        ps_latch_wait(s->pt, e->id);
        e->waiters--;
    }
}

/*
 * Hold the latch to change the waiter count. A nonzero count keeps e alive
 * when the latch is dropped.
 */
static void hold(ps_entry *e)
{
    e->waiters++;
}

static void unhold(ps_store *s, ps_entry *e)
{
    PS_ASSERT(e->waiters > 0);
    e->waiters--;
    ps_entry_maybe_release(s->pt, e);
}

typedef enum op_owner_state {
    OP_NONE,
    OP_OTHER,
    OP_MINE
} op_owner_state;

/* Call with no other locks held. gate_mu protects op_owner even on misuse. */
static op_owner_state op_owner_check(ps_store *s)
{
    pthread_mutex_lock(&s->gate_mu);
    op_owner_state st = OP_NONE;
    if (atomic_load_explicit(&s->op_active, memory_order_relaxed)) {
        st = pthread_equal(s->op_owner, pthread_self()) ? OP_MINE : OP_OTHER;
    }
    pthread_mutex_unlock(&s->gate_mu);
    return st;
}

bool ps_op_active_here(ps_store *s)
{
    return op_owner_check(s) == OP_MINE;
}

bool ps_op_in_progress(ps_store *s)
{
    return op_owner_check(s) != OP_NONE;
}

static void log_not_owner(ps_store *s, op_owner_state st, const char *what)
{
    if (st == OP_NONE) {
        PS_LOG_ERROR(&s->logger, "%s outside an operation", what);
    } else {
        PS_LOG_ERROR(&s->logger, "%s from a thread that does not own the operation", what);
    }
}

/* Validate ownership before touching the writer's state. */
static int op_enter(ps_store *s, const char *what)
{
    const op_owner_state st = op_owner_check(s);
    if (st != OP_MINE) {
        log_not_owner(s, st, what);
        return EINVAL;
    }
    if (atomic_load_explicit(&s->failed, memory_order_acquire)) {
        return EIO;
    }
    return 0;
}

/* User calls can address neither group bitmaps nor the reserved UINT32_MAX. */
static int check_user_id(ps_store *s, ps_page_id id, const char *what)
{
    if (id.raw == UINT32_MAX || ps_is_bitmap_id(s, id)) {
        PS_LOG_ERROR(&s->logger, "%s: page %u is not a user page", what, id.raw);
        return EINVAL;
    }
    return 0;
}

/* Writer only. Bitmap pages stay resident, so this check needs no page-in. */
static bool committed_allocated(ps_store *s, ps_page_id id)
{
    const u32 g = ps_group_of(s, id);
    if (g >= ps_allocator_num_groups(s->alloc)) {
        return false;
    }
    const ps_page_id bid = ps_bitmap_id(s, g);
    ps_latch(s->pt, bid);
    const ps_entry *e = ps_entry_find(s->pt, bid);
    PS_ASSERT(e != NULL && e->head != NULL && e->head->frame != PS_NO_FRAME);
    const u8 *bits = (const u8 *) ps_node_page(s, e->head) + PS_PAGE_HEADER_SIZE;
    const bool set = ps_bit_test(bits, id.raw % s->gsize);
    ps_unlatch(s->pt, bid);
    return set;
}

/* Find the resources this operation already reserved for group g. */
static ps_op_group *find_group(ps_store *s, u32 g)
{
    for (u32 i = 0; i < s->op.ngroups; i++) {
        if (s->op.groups[i].g == g) {
            return &s->op.groups[i];
        }
    }
    return NULL;
}

/* Leave a hole in the page list so commit can skip a cancelled allocation. */
static void drop_page(ps_store *s, const ps_entry *e)
{
    for (u32 i = s->op.npages; i-- > 0;) {
        if (s->op.pages[i] == e) {
            s->op.pages[i] = NULL;
            return;
        }
    }
    PS_ASSERT(false);
}

/* Hold the page latch with v->entry already set. */
static void attach_frame(ps_store *s, ps_vnode *v, u32 f)
{
    v->frame = f;
    ps_frames_set_owner(s->frames, f, v, v->entry->id);
}

/* Also counts frames promised to the outer page call during bitmap lookup. */
static bool op_room(const ps_store *s, u32 n)
{
    return (u64) s->op.frames + s->op.reserved + n <= ps_op_limit(s);
}

/* ---- the allocator's bitmap callback -------------------------------------- */

/*
 * On first touch, reserve a frame and node so commit can copy the bitmap
 * without allocating. The allocator calls this with no latch held.
 */
static int bitmap_get(ps_bitmap_get_ctx *ctx, u32 g, const u8 **bits)
{
    ps_store *s = ctx->s;
    PS_ASSERT(find_group(s, g) == NULL);
    if (!op_room(s, 1)) {
        ctx->no_room = true;
        return ENOBUFS;
    }
    int rv = reserve_groups(s, s->op.ngroups + 1);
    if (rv != 0) {
        return rv;
    }
    ps_vnode *spare_node = NULL;
    rv = ps_vnode_new(s->pt, &spare_node);
    if (rv != 0) {
        return rv;
    }
    u32 spare = PS_NO_FRAME;
    rv = ps_mem_frame(s, true, &spare);
    if (rv != 0) {
        ps_vnode_delete(s->pt, spare_node);
        return rv;
    }

    const ps_page_id id = ps_bitmap_id(s, g);
    ps_entry *e = latch_find(s, id);
    if (e == NULL) {
        rv = ps_entry_get(s->pt, id, &e);
    }
    ps_vnode *v = NULL;
    if (rv == 0) {
        e->flags |= PS_E_BITMAP;
        ps_vnode *h = e->head;
        if (h != NULL && h->frame != PS_NO_FRAME) {
            ps_node_pin(s, h);
            v = h;
        } else {
            rv = ps_page_in(s, e, h, true, &v);
        }
    }
    if (rv != 0) {
        /* A failed page-in has already released e if it is empty. */
        ps_unlatch(s->pt, id);
        ps_mem_release(s, spare);
        ps_vnode_delete(s->pt, spare_node);
        return rv;
    }
    ps_update_evictable(s, e);
    ps_unlatch(s->pt, id);

    ps_op_group *r = &s->op.groups[s->op.ngroups++];
    r->g = g;
    r->is_new = false;
    r->e = e;
    r->v = v;
    r->spare = spare;
    r->spare_node = spare_node;
    s->op.frames++;
    *bits = (const u8 *) ps_node_page(s, v) + PS_PAGE_HEADER_SIZE;
    return 0;
}

/* Prepare the next group's entry and node before changing the allocator. */
static int reserve_kit(ps_store *s)
{
    if (s->op.kit_entry != NULL) {
        return 0;
    }
    const u32 g = ps_allocator_num_groups(s->alloc) + s->op.new_groups;
    if (g >= s->max_groups) {
        return 0; /* no group can be created; the allocator reports ENOSPC */
    }
    ps_vnode *v = NULL;
    int rv = ps_vnode_new(s->pt, &v);
    if (rv != 0) {
        return rv;
    }
    const ps_page_id id = ps_bitmap_id(s, g);
    ps_entry *e = latch_find(s, id);
    if (e == NULL) {
        rv = ps_entry_get(s->pt, id, &e);
    }
    if (rv != 0) {
        ps_unlatch(s->pt, id);
        ps_vnode_delete(s->pt, v);
        return rv;
    }
    PS_ASSERT(e->head == NULL);
    hold(e);
    ps_unlatch(s->pt, id);
    s->op.kit_entry = e;
    s->op.kit_node = v;
    s->op.kit_group = g;
    return 0;
}

/* Cancellation cannot fail because the group is already prepared. */
static void cancel_alloc(ps_store *s, ps_page_id id)
{
    ps_bitmap_get_ctx gc = {s, false};
    bool cancelled = false;
    const int rv = ps_allocator_free(s->alloc, bitmap_get, &gc, id, &cancelled);
    PS_ASSERT(rv == 0 && cancelled);
    PS_UNUSED(rv);
}

/* ---- begin / commit / abort ----------------------------------------------- */

/* Reuse the arrays from the previous operation. */
static void op_start(ps_store *s, ps_lsn lsn)
{
    ps_op *op = &s->op;
    op->lsn = lsn;
    op->frames = 0;
    op->reserved = 0;
    op->npages = 0;
    op->npins = 0;
    op->ngroups = 0;
    op->new_groups = 0;
    op->kit_entry = NULL;
    op->kit_node = NULL;
    op->kit_group = 0;
    ps_allocator_op_begin(s->alloc);
}

/* The caller checks for nesting before this wait to avoid a self-deadlock. */
static void gate_enter_locked(ps_store *s)
{
    while (atomic_load_explicit(&s->op_active, memory_order_relaxed)) {
        pthread_cond_wait(&s->gate_cv, &s->gate_mu);
    }
}

static void gate_leave(ps_store *s)
{
    pthread_mutex_lock(&s->gate_mu);
    atomic_store_explicit(&s->op_active, false, memory_order_release);
    pthread_cond_broadcast(&s->gate_cv);
    pthread_mutex_unlock(&s->gate_mu);
}

int ps_op_begin(ps_store *s, ps_lsn lsn)
{
    pthread_mutex_lock(&s->gate_mu);
    if (atomic_load_explicit(&s->op_active, memory_order_relaxed) &&
        pthread_equal(s->op_owner, pthread_self())) {
        pthread_mutex_unlock(&s->gate_mu);
        PS_LOG_ERROR(
            &s->logger,
            "ps_op_begin(%llu): this thread already has an operation",
            (unsigned long long) lsn.raw);
        return EINVAL;
    }
    gate_enter_locked(s);
    if (atomic_load_explicit(&s->failed, memory_order_acquire)) {
        pthread_mutex_unlock(&s->gate_mu);
        return EIO;
    }
    const ps_lsn committed =
        ps_lsn_from(atomic_load_explicit(&s->committed_lsn, memory_order_acquire));
    if (ps_lsn_le(lsn, committed)) {
        pthread_mutex_unlock(&s->gate_mu);
        PS_LOG_ERROR(
            &s->logger,
            "ps_op_begin(%llu): LSN must exceed the committed LSN %llu",
            (unsigned long long) lsn.raw,
            (unsigned long long) committed.raw);
        return EINVAL;
    }
    s->op_owner = pthread_self();
    atomic_store_explicit(&s->op_active, true, memory_order_release);
    pthread_mutex_unlock(&s->gate_mu);
    op_start(s, lsn);
    return 0;
}

void ps_op_begin_format(ps_store *s)
{
    pthread_mutex_lock(&s->gate_mu);
    PS_ASSERT(!atomic_load_explicit(&s->op_active, memory_order_relaxed));
    s->op_owner = pthread_self();
    atomic_store_explicit(&s->op_active, true, memory_order_release);
    pthread_mutex_unlock(&s->gate_mu);
    op_start(s, PS_LSN_NONE);
}

/* Commit calls this under reg_mu. Abort needs only the individual latches. */
static void op_release(ps_store *s)
{
    ps_op *op = &s->op;
    for (u32 i = 0; i < op->npins; i++) {
        ps_vnode *v = op->pins[i];
        ps_entry *e = v->entry;
        const ps_page_id id = e->id;
        ps_latch(s->pt, id);
        PS_ASSERT((v->flags & PS_VF_OP_PIN) != 0);
        v->flags &= (u8) ~PS_VF_OP_PIN;
        ps_node_unpin(s, e, v);
        ps_unlatch(s->pt, id);
    }
    op->npins = 0;

    for (u32 i = 0; i < op->ngroups; i++) {
        ps_op_group *r = &op->groups[i];
        const ps_page_id id = ps_bitmap_id(s, r->g);
        ps_latch(s->pt, id);
        if (r->is_new) {
            ps_vnode_delete(s->pt, r->v); /* NULL once commit consumed it */
            unhold(s, r->e);
        } else {
            ps_node_unpin(s, r->e, r->v);
            ps_vnode_delete(s->pt, r->spare_node);
        }
        ps_unlatch(s->pt, id);
        if (r->spare != PS_NO_FRAME) {
            ps_mem_release(s, r->spare);
        }
    }
    op->ngroups = 0;

    if (op->kit_entry != NULL) {
        const ps_page_id id = ps_bitmap_id(s, op->kit_group);
        ps_latch(s->pt, id);
        unhold(s, op->kit_entry);
        ps_unlatch(s->pt, id);
        ps_vnode_delete(s->pt, op->kit_node);
        op->kit_entry = NULL;
        op->kit_node = NULL;
    }
    op->npages = 0;
    op->frames = 0;
}

/*
 * Hold reg_mu. A checkpoint can select a bitmap before it copies the
 * bytes. Put the update in the reserved frame when the checkpoint can
 * see the head, so its original image stays intact.
 */
static void commit_group(ps_store *s, ps_op_group *r)
{
    const ps_lsn lsn = s->op.lsn;
    const ps_page_id id = ps_bitmap_id(s, r->g);
    ps_entry *e = r->e;
    ps_latch(s->pt, id);
    ps_vnode *v = r->v;
    ps_page *p;
    if (r->is_new) {
        PS_ASSERT(e->head == NULL);
        p = (ps_page *) ps_frame(s->frames, r->spare);
        memset(p, 0, s->page_size);
        p->magic = PS_BITMAP_MAGIC;
        p->id = id;
        p->flags = PS_PAGE_FLAG_BITMAP;
        v->entry = e;
        v->kind = PS_V_DATA;
        v->flags = 0;
        attach_frame(s, v, r->spare);
        e->head = v;
        e->flags |= PS_E_BITMAP;
        r->v = NULL;
        r->spare = PS_NO_FRAME;
    } else {
        PS_ASSERT(e->head == v);
        if (ps_allocator_group_unchanged(s->alloc, r->g)) {
            /* Cancelled allocations alone leave the bitmap's LSN as it was. */
            ps_unlatch(s->pt, id);
            return;
        }
        if (s->ckpt_running && ps_lsn_le(v->lsn, s->ckpt_lsn)) {
            ps_vnode *c = r->spare_node;
            PS_ASSERT(c != NULL && r->spare != PS_NO_FRAME);
            p = (ps_page *) ps_frame(s->frames, r->spare);
            memcpy(p, ps_node_page(s, v), s->page_size);
            c->entry = e;
            c->kind = PS_V_DATA;
            c->flags = 0;
            attach_frame(s, c, r->spare);
            c->older = v;
            e->head = c;
            v->retire_next = s->bmcopy_head;
            s->bmcopy_head = v;
            r->spare_node = NULL; /* The operation's pin stays on the old version. */
            r->spare = PS_NO_FRAME;
            v = c;
        } else {
            p = ps_node_page(s, v);
            v->flags &= (u8) ~PS_VF_ON_DISK;
        }
    }
    ps_allocator_apply(s->alloc, r->g, (u8 *) p + PS_PAGE_HEADER_SIZE);
    p->lsn = lsn;
    v->lsn = lsn;
    ps_mark_dirty(s, e);
    ps_update_evictable(s, e);
    ps_unlatch(s->pt, id);
}

void ps_op_commit(ps_store *s)
{
    const op_owner_state st = op_owner_check(s);
    if (st != OP_MINE) {
        log_not_owner(s, st, "ps_op_commit");
        PS_ASSERT(!"ps_op_commit without owning an operation");
        return;
    }
    ps_op *op = &s->op;

    pthread_mutex_lock(&s->reg_mu);

    const u32 *groups = NULL;
    const u32 ng = ps_allocator_op_groups(s->alloc, &groups);
    for (u32 i = 0; i < ng; i++) {
        ps_op_group *r = find_group(s, groups[i]);
        PS_ASSERT(r != NULL);
        commit_group(s, r);
    }

    for (u32 i = 0; i < op->npages; i++) {
        ps_entry *e = op->pages[i];
        if (e == NULL) {
            continue;
        }
        const ps_page_id id = e->id;
        ps_latch(s->pt, id);
        ps_vnode *h = e->head;
        PS_ASSERT(h != NULL && (h->flags & PS_VF_IN_OP) != 0);
        h->flags &= (u8) ~PS_VF_IN_OP;
        if (h->older != NULL) {
            h->older->flags &= (u8) ~(PS_VF_OP_BASE | PS_VF_OP_HELD);
        }
        ps_mark_dirty(s, e);
        ps_update_evictable(s, e);
        ps_unlatch(s->pt, id);
    }

    ps_allocator_op_commit(s->alloc);
    atomic_store_explicit(&s->num_groups, ps_allocator_num_groups(s->alloc), memory_order_relaxed);
    atomic_store_explicit(
        &s->allocated_pages,
        ps_allocator_allocated(s->alloc),
        memory_order_relaxed);
    atomic_store_explicit(&s->committed_lsn, op->lsn.raw, memory_order_release);

    /*
     * Registration shares reg_mu with commit, so a reader cannot register
     * at an old LSN after we decide which versions to keep.
     */
    for (u32 i = 0; i < op->npages; i++) {
        ps_entry *e = op->pages[i];
        if (e == NULL) {
            continue;
        }
        const ps_page_id id = e->id;
        ps_latch(s->pt, id);
        ps_vnode *old = e->head->older;
        if (old != NULL && ps_reg_any_at_or_above(s, old->lsn)) {
            ps_retire_push(s, old, op->lsn);
        } else if (old != NULL) {
            ps_node_free(s, e, old); /* e is dirty, so it stays */
        }
        ps_unlatch(s->pt, id);
    }
    ps_retire_process(s);

    op_release(s);
    pthread_mutex_unlock(&s->reg_mu);
    ps_ckpt_note_commit(s);
    gate_leave(s);
}

void ps_op_abort(ps_store *s)
{
    const op_owner_state st = op_owner_check(s);
    if (st != OP_MINE) {
        log_not_owner(s, st, "ps_op_abort");
        PS_ASSERT(!"ps_op_abort without owning an operation");
        return;
    }
    ps_op *op = &s->op;

    for (u32 i = op->npages; i-- > 0;) {
        ps_entry *e = op->pages[i];
        if (e == NULL) {
            continue;
        }
        const ps_page_id id = e->id;
        ps_latch(s->pt, id);
        /*
         * A snapshot can be loading a version below the operation's head.
         * Wait before changing the chain it will return to.
         */
        wait_loaded(s, e);
        ps_vnode *h = e->head;
        PS_ASSERT(h != NULL && (h->flags & PS_VF_IN_OP) != 0 && h->pins == 0);
        e->head = h->older;
        if (h->frame != PS_NO_FRAME) {
            ps_mem_release(s, h->frame);
        }
        ps_vnode_delete(s->pt, h);
        ps_vnode *b = e->head;
        if (b != NULL) {
            b->flags &= (u8) ~PS_VF_OP_HELD;
        }
        if (b != NULL && (b->flags & PS_VF_OP_BASE) != 0) {
            /*
             * With the free cancelled, the disk node is the committed head
             * again. Keep it while pinned so later readers can still find it.
             */
            PS_ASSERT(b->older == NULL && (b->flags & PS_VF_RETIRED) == 0);
            b->flags &= (u8) ~PS_VF_OP_BASE;
            if (b->pins == 0) {
                ps_node_free(s, e, b);
                ps_unlatch(s->pt, id);
                continue;
            }
        }
        /*
         * A checkpoint may have made the marker below the operation's head
         * durable meanwhile, so it can now be dropped.
         */
        ps_drop_lone_head(s, e);
        ps_update_evictable(s, e);
        ps_entry_maybe_release(s->pt, e);
        ps_unlatch(s->pt, id);
    }
    op_release(s);
    ps_allocator_op_abort(s->alloc);
    gate_leave(s);
}

/* ---- page calls ---------------------------------------------------------- */

/*
 * On success the caller owns the latch, plus one pin if the head is
 * committed. Checks the operation limit before page-in, counting a write's
 * copy and any committed frame the operation would start holding.
 */
static int writer_head(
    ps_store *s,
    ps_page_id id,
    const char *what,
    bool for_write,
    ps_entry **eo,
    ps_vnode **vo)
{
    bool checked = false;
    for (;;) {
        ps_entry *e = latch_find(s, id);
        ps_vnode *h = e != NULL ? e->head : NULL;
        if (h != NULL && h->kind == PS_V_FREE) {
            ps_unlatch(s->pt, id);
            PS_LOG_ERROR(&s->logger, "%s: page %u is freed", what, id.raw);
            return EINVAL;
        }
        if (h != NULL && (h->flags & PS_VF_IN_OP) != 0) {
            *eo = e;
            *vo = h;
            return 0;
        }
        if (!checked) {
            /*
             * The bitmap decides whether the ID exists. A snapshot may have
             * loaded stale bytes for a freed ID after its marker was dropped.
             * Only this writer can change the bitmap, so check it once.
             */
            ps_unlatch(s->pt, id);
            if (!committed_allocated(s, id)) {
                PS_LOG_ERROR(&s->logger, "%s: page %u is not allocated", what, id.raw);
                return EINVAL;
            }
            checked = true;
            continue;
        }
        const bool counted = h != NULL && (h->flags & PS_VF_OP_PIN) != 0;
        if (!op_room(s, (counted ? 0u : 1u) + (for_write ? 1u : 0u))) {
            ps_unlatch(s->pt, id);
            return ENOBUFS;
        }
        if (h != NULL && h->frame != PS_NO_FRAME) {
            ps_node_pin(s, h);
            ps_update_evictable(s, e);
            ps_frames_touch(s->frames, h->frame);
            atomic_fetch_add_explicit(&s->cache_hits, 1, memory_order_relaxed);
            *eo = e;
            *vo = h;
            return 0;
        }
        int rv = 0;
        if (e == NULL) {
            rv = ps_entry_get(s->pt, id, &e);
        }
        ps_vnode *v = NULL;
        if (rv == 0) {
            rv = ps_page_in(s, e, h, true, &v);
        }
        if (rv != 0) {
            ps_unlatch(s->pt, id);
            return rv;
        }
        *eo = e;
        *vo = v;
        return 0;
    }
}

int ps_read(ps_store *s, ps_page_id id, const ps_page **page)
{
    int rv = op_enter(s, "ps_read");
    if (rv == 0) {
        rv = check_user_id(s, id, "ps_read");
    }
    if (rv == 0) {
        rv = reserve_pins(s, s->op.npins + 1);
    }
    if (rv != 0) {
        return rv;
    }
    ps_entry *e = NULL;
    ps_vnode *v = NULL;
    rv = writer_head(s, id, "ps_read", false, &e, &v);
    if (rv != 0) {
        return rv;
    }
    if ((v->flags & PS_VF_IN_OP) == 0) {
        if ((v->flags & PS_VF_OP_PIN) != 0) {
            ps_node_unpin(s, e, v); /* Keep one pin across repeated reads. */
        } else {
            v->flags |= PS_VF_OP_PIN;
            s->op.pins[s->op.npins++] = v;
            s->op.frames++;
        }
    }
    *page = ps_node_page(s, v);
    ps_unlatch(s->pt, id);
    return 0;
}

int ps_write(ps_store *s, ps_page_id id, ps_page **page)
{
    int rv = op_enter(s, "ps_write");
    if (rv == 0) {
        rv = check_user_id(s, id, "ps_write");
    }
    if (rv == 0) {
        rv = reserve_pages(s, s->op.npages + 1);
    }
    if (rv != 0) {
        return rv;
    }
    ps_entry *e = NULL;
    ps_vnode *old = NULL;
    rv = writer_head(s, id, "ps_write", true, &e, &old);
    if (rv != 0) {
        return rv;
    }
    if ((old->flags & PS_VF_IN_OP) != 0) {
        *page = ps_node_page(s, old);
        ps_unlatch(s->pt, id);
        return 0;
    }
    /* The pin keeps old resident while we get a frame with the latch dropped. */
    ps_unlatch(s->pt, id);

    ps_vnode *v = NULL;
    u32 f = PS_NO_FRAME;
    rv = ps_vnode_new(s->pt, &v);
    if (rv == 0) {
        rv = ps_mem_frame(s, true, &f);
    }
    if (rv != 0) {
        ps_vnode_delete(s->pt, v);
        ps_latch(s->pt, id);
        ps_node_unpin(s, e, old);
        ps_unlatch(s->pt, id);
        return rv;
    }
    ps_page *p = (ps_page *) ps_frame(s->frames, f);
    memcpy(p, ps_node_page(s, old), s->page_size);
    p->lsn = s->op.lsn;

    ps_latch(s->pt, id);
    PS_ASSERT(e->head == old);
    v->lsn = s->op.lsn;
    v->entry = e;
    v->kind = PS_V_DATA;
    v->flags = PS_VF_IN_OP;
    attach_frame(s, v, f);
    v->older = old;
    e->head = v;
    s->op.frames++;
    if ((old->flags & PS_VF_OP_PIN) == 0) {
        /* Count old too, since CLOCK can only evict a chain head. */
        old->flags |= PS_VF_OP_HELD;
        s->op.frames++;
    }
    ps_node_unpin(s, e, old);
    ps_update_evictable(s, e);
    ps_unlatch(s->pt, id);

    s->op.pages[s->op.npages++] = e;
    *page = p;
    return 0;
}

/*
 * Reusing an ID lets a later checkpoint overwrite the disk copy. Load
 * any old disk version a reader can still see before reusing it. These
 * frames go against the retire queue's count, so cut-off can reclaim them.
 * Enter and return with e latched. The caller can release e on failure.
 */
static int load_frameless(ps_store *s, ps_entry *e)
{
    for (;;) {
        wait_loaded(s, e);
        ps_vnode *v = e->head;
        while (v != NULL &&
               !(v->kind == PS_V_DATA && v->frame == PS_NO_FRAME &&
                 (v->flags & PS_VF_FREE_WHEN_UNPINNED) == 0)) {
            v = v->older;
        }
        if (v == NULL) {
            return 0;
        }
        hold(e); /* a failed page-in may free v and so empty the chain */
        ps_vnode *got = NULL;
        const int rv = ps_page_in(s, e, v, true, &got);
        if (rv == 0) {
            ps_node_unpin(s, e, got);
        }
        PS_ASSERT(e->waiters > 0);
        e->waiters--;
        if (rv != 0) {
            return rv;
        }
    }
}

int ps_alloc(ps_store *s, ps_page_id *id_out, ps_page **page)
{
    int rv = op_enter(s, "ps_alloc");
    if (rv == 0) {
        rv = reserve_pages(s, s->op.npages + 1);
    }
    if (rv == 0) {
        /* One record for the callback, one for a group this call may create. */
        rv = reserve_groups(s, s->op.ngroups + 2);
    }
    if (rv == 0) {
        rv = reserve_kit(s);
    }
    if (rv == 0 && !op_room(s, 1)) {
        rv = ENOBUFS;
    }
    if (rv != 0) {
        return rv;
    }
    ps_vnode *v = NULL;
    u32 f = PS_NO_FRAME;
    rv = ps_vnode_new(s->pt, &v);
    if (rv == 0) {
        rv = ps_mem_frame(s, true, &f);
    }
    if (rv != 0) {
        ps_vnode_delete(s->pt, v);
        return rv;
    }
    s->op.frames++;

    ps_bitmap_get_ctx gc = {s, false};
    ps_page_id id = PS_PAGE_ID_NONE;
    bool new_group = false;
    rv = ps_allocator_next(s->alloc, bitmap_get, &gc, &id, &new_group);
    if (rv == ENOBUFS && !gc.no_room) {
        /* The allocator's group limit (set at open): another bitmap page would not fit. */
        PS_LOG_ERROR(
            &s->logger,
            "ps_alloc: every page of the %u groups is in use and a new group's bitmap page "
            "would leave an operation fewer than %u of the %u cache frames; reopen with a "
            "larger cache",
            ps_allocator_num_groups(s->alloc) + s->op.new_groups,
            PS_OP_MIN_FRAMES,
            s->total_frames);
    }
    if (rv != 0) {
        ps_mem_release(s, f);
        s->op.frames--;
        ps_vnode_delete(s->pt, v);
        return rv;
    }

    if (new_group) {
        /*
         * The allocator has already created the group. If a second frame
         * is unavailable, give the bitmap the reserved frame and cancel
         * the user page allocation.
         */
        PS_ASSERT(s->op.kit_entry != NULL && ps_group_of(s, id) == s->op.kit_group);
        u32 bf = PS_NO_FRAME;
        rv = op_room(s, 1) ? ps_mem_frame(s, true, &bf) : ENOBUFS;
        if (rv == 0) {
            s->op.frames++;
        } else {
            bf = f;
            f = PS_NO_FRAME;
        }
        ps_op_group *r = &s->op.groups[s->op.ngroups++];
        r->g = s->op.kit_group;
        r->is_new = true;
        r->e = s->op.kit_entry;
        r->v = s->op.kit_node;
        r->spare = bf;
        r->spare_node = NULL;
        s->op.kit_entry = NULL;
        s->op.kit_node = NULL;
        s->op.new_groups++;
        if (rv != 0) {
            cancel_alloc(s, id);
            ps_vnode_delete(s->pt, v);
            return rv;
        }
    }

    ps_entry *e = latch_find(s, id);
    if (e == NULL) {
        rv = ps_entry_get(s->pt, id, &e);
        if (rv != 0) {
            ps_unlatch(s->pt, id);
            cancel_alloc(s, id);
            ps_mem_release(s, f);
            s->op.frames--;
            ps_vnode_delete(s->pt, v);
            return rv;
        }
    }
    PS_ASSERT(e->head == NULL || (e->head->flags & PS_VF_IN_OP) == 0);
    rv = load_frameless(s, e);
    if (rv != 0) {
        ps_entry_maybe_release(s->pt, e);
        ps_unlatch(s->pt, id);
        cancel_alloc(s, id);
        ps_mem_release(s, f);
        s->op.frames--;
        ps_vnode_delete(s->pt, v);
        return rv;
    }
    ps_page *p = (ps_page *) ps_frame(s->frames, f);
    memset(p, 0, s->page_size);
    p->id = id;
    p->lsn = s->op.lsn;
    v->lsn = s->op.lsn;
    v->entry = e;
    v->kind = PS_V_DATA;
    v->flags = PS_VF_IN_OP;
    attach_frame(s, v, f);
    v->older = e->head;
    e->head = v;
    ps_update_evictable(s, e);
    ps_unlatch(s->pt, id);

    s->op.pages[s->op.npages++] = e;
    *id_out = id;
    *page = p;
    return 0;
}

/*
 * CLOCK cannot reach a clean frame below the free marker, so release it
 * here if unpinned. Checkpoint skips freed pages, leaving the disk copy
 * available until ps_alloc needs to reload it before reuse. Hold the latch.
 */
static void release_base(ps_store *s, ps_vnode *base)
{
    if (base == NULL || base->frame == PS_NO_FRAME || base->pins > 0 ||
        (base->flags & PS_VF_ON_DISK) == 0) {
        return;
    }
    PS_ASSERT((base->flags & PS_VF_RETIRED) == 0);
    if ((base->flags & PS_VF_OP_HELD) != 0) {
        base->flags &= (u8) ~PS_VF_OP_HELD;
        s->op.frames--;
    }
    ps_mem_release(s, base->frame);
    base->frame = PS_NO_FRAME;
}

int ps_free(ps_store *s, ps_page_id id)
{
    int rv = op_enter(s, "ps_free");
    if (rv == 0) {
        rv = check_user_id(s, id, "ps_free");
    }
    if (rv == 0) {
        rv = reserve_pages(s, s->op.npages + 1);
    }
    if (rv == 0) {
        rv = reserve_groups(s, s->op.ngroups + 1);
    }
    if (rv != 0) {
        return rv;
    }
    /* Reserve a disk node too, in case the page has never been loaded. */
    ps_vnode *m = NULL;
    ps_vnode *b = NULL;
    rv = ps_vnode_new(s->pt, &m);
    if (rv == 0) {
        rv = ps_vnode_new(s->pt, &b);
    }
    ps_entry *e = NULL;
    u32 held = 0; /* 1 if the committed head will count against the operation limit */
    if (rv == 0) {
        e = latch_find(s, id);
        if (e == NULL) {
            rv = ps_entry_get(s->pt, id, &e);
        }
        const ps_vnode *h = rv == 0 ? e->head : NULL;
        /*
         * Only a dirty base needs room. A checkpoint may clean it before
         * we install the marker, and a clean base stays clean.
         */
        held = h != NULL && h->kind == PS_V_DATA && h->frame != PS_NO_FRAME &&
                (h->flags & (PS_VF_IN_OP | PS_VF_ON_DISK | PS_VF_OP_PIN)) == 0 ?
            1 :
            0;
        if (rv == 0 && h != NULL && h->kind == PS_V_FREE) {
            PS_LOG_ERROR(&s->logger, "ps_free: page %u is already freed", id.raw);
            ps_unlatch(s->pt, id);
            rv = EINVAL;
        } else if (rv == 0 && !op_room(s, held)) {
            ps_entry_maybe_release(s->pt, e);
            ps_unlatch(s->pt, id);
            rv = ENOBUFS;
        } else if (rv == 0) {
            /* The allocator may run the bitmap callback, which takes another latch. */
            hold(e);
            ps_unlatch(s->pt, id);
        } else {
            ps_unlatch(s->pt, id);
        }
    }
    if (rv != 0) {
        ps_vnode_delete(s->pt, m);
        ps_vnode_delete(s->pt, b);
        return rv;
    }

    ps_bitmap_get_ctx gc = {s, false};
    bool cancelled = false;
    s->op.reserved = held; /* a spare the callback takes must leave room for it */
    rv = ps_allocator_free(s->alloc, bitmap_get, &gc, id, &cancelled);
    s->op.reserved = 0;

    ps_latch(s->pt, id);
    e->waiters--;
    if (rv != 0) {
        ps_entry_maybe_release(s->pt, e);
        ps_unlatch(s->pt, id);
        ps_vnode_delete(s->pt, m);
        ps_vnode_delete(s->pt, b);
        if (rv == EINVAL) {
            PS_LOG_ERROR(&s->logger, "ps_free: page %u is not allocated", id.raw);
        }
        return rv;
    }
    wait_loaded(s, e);

    ps_vnode *h = e->head;
    if (cancelled) {
        /* An allocation cancelled in its own operation leaves no free marker. */
        PS_ASSERT(h != NULL && (h->flags & PS_VF_IN_OP) != 0 && h->kind == PS_V_DATA);
        e->head = h->older;
        ps_mem_release(s, h->frame);
        ps_vnode_delete(s->pt, h);
        s->op.frames--;
        drop_page(s, e);
        ps_drop_lone_head(s, e);
        ps_update_evictable(s, e);
        ps_entry_maybe_release(s->pt, e);
    } else if (h != NULL && (h->flags & PS_VF_IN_OP) != 0) {
        /* This operation's copy becomes the marker. The committed version stays replaced. */
        PS_ASSERT(h->kind == PS_V_DATA && h->pins == 0);
        ps_mem_release(s, h->frame);
        h->frame = PS_NO_FRAME;
        h->kind = PS_V_FREE;
        s->op.frames--;
        release_base(s, h->older);
        ps_update_evictable(s, e);
    } else {
        if (h == NULL) {
            /* PS_LSN_NONE lets readers find the disk node before its first load. */
            b->lsn = PS_LSN_NONE;
            b->entry = e;
            b->kind = PS_V_DATA;
            b->flags = PS_VF_ON_DISK | PS_VF_OP_BASE;
            e->head = b;
            b = NULL;
        }
        ps_vnode *base = e->head;
        PS_ASSERT(base->kind == PS_V_DATA);
        m->lsn = s->op.lsn;
        m->entry = e;
        m->kind = PS_V_FREE;
        m->flags = PS_VF_IN_OP;
        m->older = base;
        e->head = m;
        m = NULL;
        if (base->frame != PS_NO_FRAME && (base->flags & (PS_VF_ON_DISK | PS_VF_OP_PIN)) == 0) {
            /* A dirty base has no current disk copy to fall back on. */
            PS_ASSERT(held == 1);
            base->flags |= PS_VF_OP_HELD;
            s->op.frames++;
        } else {
            release_base(s, base);
        }
        ps_update_evictable(s, e);
        s->op.pages[s->op.npages++] = e;
    }
    ps_unlatch(s->pt, id);
    ps_vnode_delete(s->pt, m);
    ps_vnode_delete(s->pt, b);
    return 0;
}

/* ---- pressure ------------------------------------------------------------ */

void ps_op_pressure(const ps_store *s, ps_pressure *out)
{
    out->op_frames = s->op.frames;
    out->op_limit = ps_op_limit(s);
    out->dirty_frames = atomic_load_explicit(&s->frames_dirty, memory_order_relaxed);
    out->total_frames = s->total_frames;
}
