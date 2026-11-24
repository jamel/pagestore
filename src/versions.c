#include "frames.h"
#include "ptable.h"
#include "store.h"
#include "util.h"

#include "pagestore/pagestore.h"

#include <stdatomic.h>

/*
 * Change page version chains and their frame accounting under the page
 * latch. Pins keep a version alive until callers stop using it. The
 * dirty lists keep entries alive while a checkpoint needs them.
 */

void ps_node_pin(ps_store *s, ps_vnode *v)
{
    if (v->pins++ == 0 && v->frame != PS_NO_FRAME) {
        atomic_fetch_add_explicit(&s->frames_pinned, 1, memory_order_relaxed);
    }
}

/* Detach the frame while keeping the version node and its LSN. */
static void drop_frame(ps_store *s, ps_vnode *v)
{
    if (v->frame == PS_NO_FRAME) {
        return;
    }
    /* ps_retire_process takes a node off the queue (and uncounts it) before freeing it. */
    PS_ASSERT((v->flags & PS_VF_RETIRED) == 0);
    ps_mem_release(s, v->frame);
    v->frame = PS_NO_FRAME;
}

void ps_node_free(ps_store *s, ps_entry *e, ps_vnode *v)
{
    PS_ASSERT(v->entry == e);
    if (v->pins > 0) {
        v->flags |= PS_VF_FREE_WHEN_UNPINNED;
        return;
    }
    ps_vnode **p = &e->head;
    while (*p != v) {
        PS_ASSERT(*p != NULL);
        p = &(*p)->older;
    }
    *p = v->older;
    drop_frame(s, v);
    ps_vnode_delete(s->pt, v);

    ps_drop_lone_head(s, e);
    ps_update_evictable(s, e);
    ps_entry_maybe_release(s->pt, e);
}

void ps_drop_lone_head(ps_store *s, ps_entry *e)
{
    ps_vnode *h = e->head;
    if (h == NULL || h->older != NULL || h->pins != 0 || h->flags != PS_VF_ON_DISK ||
        atomic_load_explicit(&e->dirty, memory_order_relaxed) != 0) {
        return;
    }
    /* The durable bitmap and data file already describe this page. */
    if (h->kind == PS_V_FREE || h->frame == PS_NO_FRAME) {
        e->head = NULL;
        ps_vnode_delete(s->pt, h);
    }
}

/*
 * A clean frame a snapshot loaded under an in-operation free marker. CLOCK
 * cannot reach it and the operation never counted it.
 */
static bool stray_under_marker(const ps_entry *e, const ps_vnode *v)
{
    const ps_vnode *h = e->head;
    return h != NULL && h != v && h->older == v && (h->flags & PS_VF_IN_OP) != 0 &&
        h->kind == PS_V_FREE && v->frame != PS_NO_FRAME && v->pins == 0 &&
        (v->flags & (PS_VF_ON_DISK | PS_VF_RETIRED | PS_VF_OP_PIN | PS_VF_OP_HELD)) ==
        PS_VF_ON_DISK;
}

void ps_node_unpin(ps_store *s, ps_entry *e, ps_vnode *v)
{
    PS_ASSERT(v->entry == e);
    PS_ASSERT(v->pins > 0);
    if (--v->pins > 0) {
        return;
    }
    if (v->frame != PS_NO_FRAME) {
        atomic_fetch_sub_explicit(&s->frames_pinned, 1, memory_order_relaxed);
    }
    if ((v->flags & PS_VF_FREE_WHEN_UNPINNED) != 0) {
        ps_node_free(s, e, v);
        return;
    }
    if (stray_under_marker(e, v)) {
        drop_frame(s, v); /* reloaded from disk if a snapshot reads it again */
    }
    ps_update_evictable(s, e);
}

/* Can this head be proposed for eviction? The evictor checks dirty lists too. */
static bool head_evictable(const ps_entry *e, const ps_vnode *h)
{
    return (e->flags & (PS_E_BITMAP | PS_E_LOADING)) == 0 && h->kind == PS_V_DATA &&
        (h->flags & (PS_VF_IN_OP | PS_VF_ON_DISK)) == PS_VF_ON_DISK && h->pins == 0;
}

void ps_update_evictable(ps_store *s, ps_entry *e)
{
    for (ps_vnode *v = e->head; v != NULL; v = v->older) {
        if (v->frame == PS_NO_FRAME) {
            continue;
        }
        const bool on = v == e->head && head_evictable(e, v);
        ps_frames_set_evictable(s->frames, v->frame, on);
    }
}

bool ps_evict_head(ps_store *s, ps_entry *e, u32 *f)
{
    ps_vnode *h = e->head;
    if (h == NULL || h->frame == PS_NO_FRAME || !head_evictable(e, h) ||
        atomic_load_explicit(&e->dirty, memory_order_relaxed) != 0) {
        return false;
    }
    PS_ASSERT((h->flags & ~PS_VF_ON_DISK) == 0);
    const u32 fr = h->frame;
    ps_frames_detach(s->frames, fr);
    h->frame = PS_NO_FRAME;
    if (h->older == NULL) {
        e->head = NULL;
        ps_vnode_delete(s->pt, h);
        ps_entry_maybe_release(s->pt, e);
    } else {
        /* Keep the head's LSN so snapshots can choose among the remaining versions. */
        ps_update_evictable(s, e);
    }
    *f = fr;
    return true;
}

void ps_mark_dirty(ps_store *s, ps_entry *e)
{
    const u32 i = s->next_idx;
    const u8 bit = (u8) (1u << i);
    const u8 was = atomic_fetch_or_explicit(&e->dirty, bit, memory_order_relaxed);
    if ((was & bit) != 0) {
        return;
    }
    e->dirty_next[i] = s->dirty_head[i];
    s->dirty_head[i] = e;
    if (was == 0) {
        atomic_fetch_add_explicit(&s->frames_dirty, 1, memory_order_relaxed);
    }
}
