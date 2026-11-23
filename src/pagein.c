#include "crc32c.h"
#include "store.h"
#include "util.h"

#include <errno.h>

/*
 * Load one page from disk. The LOADING flag lets other callers wait for
 * the same load while the page latch is dropped for the read.
 */

/* Check the disk image before attaching it to the version chain. */
static int check_page(ps_store *s, ps_page_id id, ps_page *p)
{
    if (!ps_page_id_eq(p->page_id, id)) {
        ps_log(s, PS_LOG_ERROR, "page %u: header has page_id %u", id.raw, p->page_id.raw);
        return EBADMSG;
    }
    const u32 stored = p->checksum;
    p->checksum = 0; /* The CRC includes this field as zero, as it is in memory. */
    const u32 crc = ps_crc32c(0, p, s->page_size);
    if (crc != stored) {
        ps_log(s, PS_LOG_ERROR, "page %u: checksum mismatch", id.raw);
        return EBADMSG;
    }
    const bool bitmap = ps_is_bitmap_id(s, id);
    const bool flagged = (p->flags & PS_PAGE_FLAG_BITMAP) != 0;
    if (bitmap != flagged || (bitmap && p->magic != PS_BITMAP_MAGIC)) {
        ps_log(s, PS_LOG_ERROR, "page %u: bad bitmap flag or magic", id.raw);
        return EBADMSG;
    }
    return 0;
}

int ps_page_in(ps_store *s, ps_entry *e, ps_vnode *frameless, bool writer, ps_vnode **out)
{
    const ps_page_id id = e->id;
    PS_ASSERT((e->flags & PS_E_LOADING) == 0);
    PS_ASSERT(frameless == NULL ? e->head == NULL : frameless->frame == PS_NO_FRAME);
    PS_ASSERT(frameless == NULL || frameless->kind == PS_V_DATA);

    e->flags |= PS_E_LOADING;
    if (frameless != NULL) {
        frameless->pins++; /* keeps it alive while the latch is dropped */
    }
    ps_unlatch(s->pt, id);

    u32 f = PS_NO_FRAME;
    ps_vnode *v = NULL;
    /*
     * A failed store reads only from memory because a fatal checkpoint can
     * leave a partly applied image on disk.
     */
    int rv = atomic_load_explicit(&s->failed, memory_order_acquire) ? EIO : 0;
    if (rv == 0) {
        rv = ps_mem_frame(s, writer, &f);
    }
    if (rv == 0 && frameless == NULL) {
        rv = ps_vnode_new(s->pt, &v);
    }
    ps_page *p = NULL;
    if (rv == 0) {
        p = (ps_page *) ps_frame(s->frames, f);
        rv = s->be.read_page(s->be.ctx, id, p);
    }
    if (rv == 0) {
        rv = check_page(s, id, p);
    }
    atomic_fetch_add_explicit(&s->cache_misses, 1, memory_order_relaxed);

    ps_latch(s->pt, id);
    e->flags &= (u16) ~PS_E_LOADING;
    ps_latch_broadcast(s->pt, id);

    if (frameless != NULL) {
        PS_ASSERT(frameless->pins > 0);
        frameless->pins--;
    }
    if (rv != 0) {
        if (f != PS_NO_FRAME) {
            ps_mem_release(s, f);
        }
        ps_vnode_delete(s->pt, v);
        if (frameless != NULL && frameless->pins == 0 &&
            (frameless->flags & PS_VF_FREE_WHEN_UNPINNED) != 0) {
            ps_node_free(s, e, frameless);
        } else {
            ps_drop_lone_head(s, e); /* older versions may have gone meanwhile */
            ps_entry_maybe_release(s->pt, e);
        }
        return rv;
    }

    /* A disk version may be too new for a snapshot, but it must be committed. */
    PS_ASSERT(p->lsn <= atomic_load(&s->committed_lsn));
    if (frameless != NULL) {
        v = frameless;
        if (v->lsn == 0) {
            /*
             * ps_free can leave a disk node without knowing its LSN. The
             * actual LSN keeps it below all the newer nodes in the chain.
             */
            v->lsn = p->lsn;
        } else {
            PS_ASSERT(p->lsn == v->lsn); /* a kept version is exactly the disk copy */
        }
    } else {
        PS_ASSERT(e->head == NULL); /* nobody changes a LOADING entry's chain */
        v->lsn = p->lsn;
        v->entry = e;
        v->kind = PS_V_DATA;
        v->flags = PS_VF_ON_DISK;
        e->head = v;
    }
    v->frame = f;
    ps_frames_set_owner(s->frames, f, v, id);
    ps_frames_touch(s->frames, f); /* Give a fresh page a CLOCK second chance. */
    if ((v->flags & PS_VF_RETIRED) != 0) {
        atomic_fetch_add_explicit(&s->frames_retired, 1, memory_order_relaxed);
    }
    ps_node_pin(s, v);
    ps_update_evictable(s, e);
    *out = v;
    return 0;
}
