#include "store.h"
#include "util.h"

#include <errno.h>

/*
 * Frame requests: free frame, eviction, cut-off, then EAGAIN or
 * ENOBUFS. See store.h for the order.
 *
 * Locks: a request is made with no page latch and without reg_mu.
 * Eviction latches one page at a time; cut-off takes reg_mu (and latches
 * inside it). A request returns EAGAIN if it needs a checkpoint.
 */

/*
 * Evicts one clean frame and returns it, still allocated, in *f.
 * The CLOCK hand only proposes a frame; whether it may really go is
 * decided under the latch of the page that owns it. The owner's page id is
 * read first without the latch; once that page is latched the id is read
 * again: a frame owned by that page cannot change owner while its latch is
 * held (frames.h), so a match makes the owner node, and every field the
 * re-check reads, stable. A failed re-check leaves the frame's ref bit set
 * by the proposal, so the next proposals move on to other frames.
 */
static bool evict_one(ps_store *s, u32 *f)
{
    for (u32 tries = 0; tries < s->total_frames; tries++) {
        u32 c = 0;
        if (ps_frames_clock(s->frames, &c) != 0) {
            return false;
        }
        const u32 id = ps_frames_owner_id(s->frames, c);
        if (id == PS_FRAMES_NO_OWNER) {
            continue; /* released or detached meanwhile */
        }
        ps_latch(s->pt, id);
        bool got = false;
        if (ps_frames_owner_id(s->frames, c) == id) {
            ps_vnode *v = ps_frames_owner(s->frames, c);
            ps_entry *e = v->entry;
            PS_ASSERT(e != NULL && e->id == id && v->frame == c);
            u32 fr = PS_NO_FRAME;
            if (e->head == v && ps_evict_head(s, e, &fr)) {
                PS_ASSERT(fr == c);
                got = true;
            }
        }
        ps_unlatch(s->pt, id);
        if (got) {
            *f = c;
            return true;
        }
    }
    return false;
}

/* A checkpoint can clean dirty frames or release versions it was holding. */
static bool checkpoint_would_help(ps_store *s)
{
    if (atomic_load_explicit(&s->frames_dirty, memory_order_relaxed) > 0) {
        return true;
    }
    pthread_mutex_lock(&s->reg_mu);
    const bool running = s->ckpt_running;
    pthread_mutex_unlock(&s->reg_mu);
    return running;
}

int ps_mem_frame(ps_store *s, bool writer, u32 *f)
{
    for (;;) {
        if (ps_frames_alloc(s->frames, f) == 0) {
            return 0;
        }
        if (evict_one(s, f)) {
            return 0;
        }
        if (writer && atomic_load_explicit(&s->frames_retired, memory_order_relaxed) > 0 &&
            ps_snap_cut_off_for_frames(s)) {
            continue;
        }
        if (checkpoint_would_help(s)) {
            /* The caller aborts, runs ps_checkpoint and retries. */
            return EAGAIN;
        }
        return ENOBUFS;
    }
}

void ps_mem_release(ps_store *s, u32 f)
{
    ps_frames_release(s->frames, f);
}
