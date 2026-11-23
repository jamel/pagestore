#include "store.h"
#include "util.h"

#include <errno.h>

/*
 * Frame requests try the free list, then CLOCK eviction. A writer can
 * also cut off snapshots, and either caller can wait for a checkpoint.
 */

/*
 * On success, *f is still allocated. A CLOCK candidate can change owners
 * before we latch its page, so recheck the ID under the latch. Proposing a
 * frame sets its ref bit, so the next pass moves past a rejected one.
 */
static bool evict_one(ps_store *s, u32 *f)
{
    for (u32 tries = 0; tries < s->total_frames; tries++) {
        u32 c = 0;
        if (ps_frames_clock(s->frames, &c) != 0) {
            return false;
        }
        const ps_page_id id = ps_frames_owner_id(s->frames, c);
        if (ps_page_id_eq(id, PS_FRAMES_NO_OWNER)) {
            continue; /* released or detached meanwhile */
        }
        ps_latch(s->pt, id);
        bool got = false;
        if (ps_page_id_eq(ps_frames_owner_id(s->frames, c), id)) {
            ps_vnode *v = ps_frames_owner(s->frames, c);
            ps_entry *e = v->entry;
            PS_ASSERT(e != NULL && ps_page_id_eq(e->id, id) && v->frame == c);
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
            if (!s->ckpt_thread_started) {
                return EAGAIN; /* Run ps_checkpoint before retrying. */
            }
            /*
             * A checkpoint already in flight can miss recent commits. A
             * fresh attempt sees all of them while the writer is blocked.
             */
            const int rv = ps_ckpt_wait(s);
            if (rv != 0) {
                return rv;
            }
            continue;
        }
        return ENOBUFS;
    }
}

void ps_mem_release(ps_store *s, u32 f)
{
    ps_frames_release(s->frames, f);
}
