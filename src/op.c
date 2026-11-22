#include "bitmap.h"
#include "store.h"
#include "util.h"

#include <errno.h>
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
static ps_entry *latch_find(ps_store *s, u32 id)
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

/* Checks the caller is inside its operation and the store is healthy. */
static int op_enter(ps_store *s, const char *what)
{
    if (!atomic_load_explicit(&s->op_active, memory_order_acquire)) {
        ps_log(s, PS_LOG_ERROR, "%s outside an operation", what);
        return EINVAL;
    }
    PS_ASSERT(pthread_equal(s->op_owner, pthread_self()));
    if (atomic_load_explicit(&s->failed, memory_order_acquire)) {
        return EIO;
    }
    return 0;
}

/* User calls can address neither group bitmaps nor the reserved UINT32_MAX. */
static int check_user_id(ps_store *s, u32 id, const char *what)
{
    if (id == UINT32_MAX || ps_is_bitmap_id(s, id)) {
        ps_log(s, PS_LOG_ERROR, "%s: page %u is not a user page", what, id);
        return EINVAL;
    }
    return 0;
}

/*
 * Writer only. Bitmap pages stay resident, so this check needs no page-in.
 */
static bool committed_allocated(ps_store *s, u32 id)
{
    const u32 g = id / s->gsize;
    if (g >= ps_allocator_num_groups(s->alloc)) {
        return false;
    }
    const u32 bid = g * s->gsize;
    ps_latch(s->pt, bid);
    const ps_entry *e = ps_entry_find(s->pt, bid);
    PS_ASSERT(e != NULL && e->head != NULL && e->head->frame != PS_NO_FRAME);
    const u8 *bits = (const u8 *) ps_node_page(s, e->head) + PS_PAGE_HEADER_SIZE;
    const bool set = ps_bit_test(bits, id % s->gsize);
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

/* Gives a resident node a frame's identity. Latch held. */
static void attach_frame(ps_store *s, ps_vnode *v, u32 f)
{
    v->frame = f;
    ps_frames_set_owner(s->frames, f, v);
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

    const u32 id = g * s->gsize;
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
    const u32 id = g * s->gsize;
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
static void cancel_alloc(ps_store *s, u32 id)
{
    ps_bitmap_get_ctx gc = {s};
    bool cancelled = false;
    const int rv = ps_allocator_free(s->alloc, bitmap_get, &gc, id, &cancelled);
    PS_ASSERT(rv == 0 && cancelled);
    PS_UNUSED(rv);
}

/* ---- begin / commit / abort ----------------------------------------------- */

/* Reuse the arrays from the previous operation. */
static void op_start(ps_store *s, u64 lsn)
{
    ps_op *op = &s->op;
    op->lsn = lsn;
    op->frames = 0;
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

int ps_op_begin(ps_store *s, u64 lsn)
{
    pthread_mutex_lock(&s->gate_mu);
    if (atomic_load_explicit(&s->op_active, memory_order_relaxed) &&
        pthread_equal(s->op_owner, pthread_self())) {
        pthread_mutex_unlock(&s->gate_mu);
        ps_log(
            s,
            PS_LOG_ERROR,
            "ps_op_begin(%llu): this thread already has an operation",
            (unsigned long long) lsn);
        return EINVAL;
    }
    gate_enter_locked(s);
    if (atomic_load_explicit(&s->failed, memory_order_acquire)) {
        pthread_mutex_unlock(&s->gate_mu);
        return EIO;
    }
    const u64 committed = atomic_load_explicit(&s->committed_lsn, memory_order_acquire);
    if (lsn <= committed) {
        pthread_mutex_unlock(&s->gate_mu);
        ps_log(
            s,
            PS_LOG_ERROR,
            "ps_op_begin(%llu): LSN must exceed the committed LSN %llu",
            (unsigned long long) lsn,
            (unsigned long long) committed);
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
    op_start(s, 0);
}

/* Commit calls this under reg_mu. Abort needs only the individual latches. */
static void op_release(ps_store *s)
{
    ps_op *op = &s->op;
    for (u32 i = 0; i < op->npins; i++) {
        ps_vnode *v = op->pins[i];
        ps_entry *e = v->entry;
        const u32 id = e->id;
        ps_latch(s->pt, id);
        PS_ASSERT((v->flags & PS_VF_OP_PIN) != 0);
        v->flags &= (u8) ~PS_VF_OP_PIN;
        ps_node_unpin(s, e, v);
        ps_unlatch(s->pt, id);
    }
    op->npins = 0;

    for (u32 i = 0; i < op->ngroups; i++) {
        ps_op_group *r = &op->groups[i];
        const u32 id = r->g * s->gsize;
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
        const u32 id = op->kit_group * s->gsize;
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

/* Applies the op's allocator changes to group r's bitmap page (commit, reg_mu held). */
static void commit_group(ps_store *s, ps_op_group *r)
{
    const u64 lsn = s->op.lsn;
    const u32 id = r->g * s->gsize;
    ps_entry *e = r->e;
    ps_latch(s->pt, id);
    ps_vnode *v = r->v;
    ps_page *p;
    if (r->is_new) {
        PS_ASSERT(e->head == NULL);
        p = (ps_page *) ps_frame(s->frames, r->spare);
        memset(p, 0, s->page_size);
        p->magic = PS_BITMAP_MAGIC;
        p->page_id = id;
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
        p = ps_node_page(s, v);
        v->flags &= (u8) ~PS_VF_ON_DISK;
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
    if (!atomic_load_explicit(&s->op_active, memory_order_acquire)) {
        PS_ASSERT(!"ps_op_commit outside an operation");
        ps_log(s, PS_LOG_ERROR, "ps_op_commit outside an operation");
        return;
    }
    PS_ASSERT(pthread_equal(s->op_owner, pthread_self()));
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
        const u32 id = e->id;
        ps_latch(s->pt, id);
        ps_vnode *h = e->head;
        PS_ASSERT(h != NULL && (h->flags & PS_VF_IN_OP) != 0);
        h->flags &= (u8) ~PS_VF_IN_OP;
        if (h->older != NULL) {
            h->older->flags &= (u8) ~PS_VF_OP_BASE;
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
    atomic_store_explicit(&s->committed_lsn, op->lsn, memory_order_release);

    /*
     * Free replaced versions. A pinned version is freed at its last unpin.
     */
    for (u32 i = 0; i < op->npages; i++) {
        ps_entry *e = op->pages[i];
        if (e == NULL) {
            continue;
        }
        const u32 id = e->id;
        ps_latch(s->pt, id);
        ps_vnode *old = e->head->older;
        if (old != NULL) {
            ps_node_free(s, e, old); /* e is dirty, so it stays */
        }
        ps_unlatch(s->pt, id);
    }

    op_release(s);
    pthread_mutex_unlock(&s->reg_mu);
    gate_leave(s);
}

void ps_op_abort(ps_store *s)
{
    if (!atomic_load_explicit(&s->op_active, memory_order_acquire)) {
        PS_ASSERT(!"ps_op_abort outside an operation");
        ps_log(s, PS_LOG_ERROR, "ps_op_abort outside an operation");
        return;
    }
    PS_ASSERT(pthread_equal(s->op_owner, pthread_self()));
    ps_op *op = &s->op;

    for (u32 i = op->npages; i-- > 0;) {
        ps_entry *e = op->pages[i];
        if (e == NULL) {
            continue;
        }
        const u32 id = e->id;
        ps_latch(s->pt, id);
        ps_vnode *h = e->head;
        PS_ASSERT(h != NULL && (h->flags & PS_VF_IN_OP) != 0 && h->pins == 0);
        e->head = h->older;
        if (h->frame != PS_NO_FRAME) {
            ps_mem_release(s, h->frame);
        }
        ps_vnode_delete(s->pt, h);
        ps_vnode *b = e->head;
        if (b != NULL && (b->flags & PS_VF_OP_BASE) != 0) {
            e->head = b->older;
            ps_vnode_delete(s->pt, b);
        }
        ps_update_evictable(s, e);
        ps_entry_maybe_release(s->pt, e);
        ps_unlatch(s->pt, id);
    }
    op_release(s);
    ps_allocator_op_abort(s->alloc);
    gate_leave(s);
}

/* ---- page calls ---------------------------------------------------------- */

/* ---- page calls ------------------------------------------------------- */

/*
 * Latches id and makes its newest version resident. On success the latch is
 * held and *vo is the head: either this op's data node (unpinned) or the
 * committed data head, which then holds one pin taken for the caller. EINVAL
 * (logged) for a free marker or an id that is not allocated; page-in errors.
 * On failure the latch is released.
 */
static int writer_head(ps_store *s, u32 id, const char *what, ps_entry **eo, ps_vnode **vo)
{
    bool checked = false;
    for (;;) {
        ps_entry *e = latch_find(s, id);
        ps_vnode *h = e != NULL ? e->head : NULL;
        if (h == NULL && !checked) {
            /* Nothing in memory: the committed bitmap says whether id exists. */
            ps_unlatch(s->pt, id);
            if (!committed_allocated(s, id)) {
                ps_log(s, PS_LOG_ERROR, "%s: page %u is not allocated", what, id);
                return EINVAL;
            }
            checked = true;
            continue;
        }
        if (h != NULL && h->kind == PS_V_FREE) {
            ps_unlatch(s->pt, id);
            ps_log(s, PS_LOG_ERROR, "%s: page %u is freed", what, id);
            return EINVAL;
        }
        if (h != NULL && (h->flags & PS_VF_IN_OP) != 0) {
            *eo = e;
            *vo = h;
            return 0;
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

int ps_read(ps_store *s, u32 id, const ps_page **page)
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
    rv = writer_head(s, id, "ps_read", &e, &v);
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

int ps_write(ps_store *s, u32 id, ps_page **page)
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
    rv = writer_head(s, id, "ps_write", &e, &old);
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
    ps_node_unpin(s, e, old);
    ps_update_evictable(s, e);
    ps_unlatch(s->pt, id);

    s->op.pages[s->op.npages++] = e;
    s->op.frames++;
    *page = p;
    return 0;
}

int ps_alloc(ps_store *s, u32 *id_out, ps_page **page)
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

    ps_bitmap_get_ctx gc = {s};
    u32 id = 0;
    bool new_group = false;
    rv = ps_allocator_next(s->alloc, bitmap_get, &gc, &id, &new_group);
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
        PS_ASSERT(s->op.kit_entry != NULL && id / s->gsize == s->op.kit_group);
        u32 bf = PS_NO_FRAME;
        rv = ps_mem_frame(s, true, &bf);
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
    ps_page *p = (ps_page *) ps_frame(s->frames, f);
    memset(p, 0, s->page_size);
    p->page_id = id;
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

int ps_free(ps_store *s, u32 id)
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
    if (rv == 0) {
        e = latch_find(s, id);
        if (e == NULL) {
            rv = ps_entry_get(s->pt, id, &e);
        }
        if (rv == 0 && e->head != NULL && e->head->kind == PS_V_FREE) {
            ps_log(s, PS_LOG_ERROR, "ps_free: page %u is already freed", id);
            ps_unlatch(s->pt, id);
            rv = EINVAL;
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

    ps_bitmap_get_ctx gc = {s};
    bool cancelled = false;
    rv = ps_allocator_free(s->alloc, bitmap_get, &gc, id, &cancelled);

    ps_latch(s->pt, id);
    e->waiters--;
    if (rv != 0) {
        ps_entry_maybe_release(s->pt, e);
        ps_unlatch(s->pt, id);
        ps_vnode_delete(s->pt, m);
        ps_vnode_delete(s->pt, b);
        if (rv == EINVAL) {
            ps_log(s, PS_LOG_ERROR, "ps_free: page %u is not allocated", id);
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
        ps_update_evictable(s, e);
        ps_entry_maybe_release(s->pt, e);
    } else if (h != NULL && (h->flags & PS_VF_IN_OP) != 0) {
        /* This operation's copy becomes the marker. The committed version stays replaced. */
        PS_ASSERT(h->kind == PS_V_DATA && h->pins == 0);
        ps_mem_release(s, h->frame);
        h->frame = PS_NO_FRAME;
        h->kind = PS_V_FREE;
        s->op.frames--;
        ps_update_evictable(s, e);
    } else {
        if (h == NULL) {
            /* A zero LSN lets readers find the disk node before its first load. */
            b->lsn = 0;
            b->entry = e;
            b->kind = PS_V_DATA;
            b->flags = PS_VF_ON_DISK | PS_VF_OP_BASE;
            e->head = b;
            b = NULL;
        }
        PS_ASSERT(e->head->kind == PS_V_DATA);
        m->lsn = s->op.lsn;
        m->entry = e;
        m->kind = PS_V_FREE;
        m->flags = PS_VF_IN_OP;
        m->older = e->head;
        e->head = m;
        m = NULL;
        ps_update_evictable(s, e);
        s->op.pages[s->op.npages++] = e;
    }
    ps_unlatch(s->pt, id);
    ps_vnode_delete(s->pt, m);
    ps_vnode_delete(s->pt, b);
    return 0;
}
