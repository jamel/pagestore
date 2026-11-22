#include "store.h"
#include "util.h"

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
    if ((v->flags & PS_VF_RETIRED) != 0) {
        atomic_fetch_sub_explicit(&s->frames_retired, 1, memory_order_relaxed);
    }
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

    /* A lone free marker that is on disk carries no information. */
    ps_vnode *h = e->head;
    if (h != NULL && h->older == NULL && h->kind == PS_V_FREE && (h->flags & PS_VF_ON_DISK) != 0 &&
        h->pins == 0) {
        e->head = NULL;
        ps_vnode_delete(s->pt, h);
    }
    ps_update_evictable(s, e);
    ps_entry_maybe_release(s->pt, e);
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
    ps_update_evictable(s, e);
}

void ps_update_evictable(ps_store *s, ps_entry *e)
{
    const bool never = (e->flags & (PS_E_BITMAP | PS_E_LOADING)) != 0;
    for (ps_vnode *v = e->head; v != NULL; v = v->older) {
        if (v->frame == PS_NO_FRAME) {
            continue;
        }
        const bool on = !never && v == e->head && v->kind == PS_V_DATA &&
            (v->flags & (PS_VF_IN_OP | PS_VF_ON_DISK)) == PS_VF_ON_DISK && v->pins == 0;
        ps_frames_set_evictable(s->frames, v->frame, on);
    }
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
    atomic_fetch_add_explicit(&s->frames_dirty, 1, memory_order_relaxed);
}
