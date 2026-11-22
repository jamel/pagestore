#include "crc32c.h"
#include "store.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/*
 * Checkpoint, the reader registry and the log hooks. The
 * retire queue is in snapshot.c.
 *
 * Locks: a checkpoint holds ckpt_mu for its whole run; it takes reg_mu
 * at its start and end, and page latches one at a time, either alone or
 * inside reg_mu. Backend calls and the hooks run with ckpt_mu only.
 *
 * What keeps the versions a checkpoint at L reads alive and unchanged
 * without holding latches across its I/O:
 * - it is registered as a reader at L, so a commit that replaces a version
 *   visible at L retires it instead of freeing it (op.c), and the retire
 *   queue frees it only after the checkpoint has unregistered;
 * - committed user versions are immutable, and the bitmap
 *   copy rule at commit (op.c) keeps a bitmap version of LSN <= L unchanged;
 * - versions in a dirty set are never evictable (they are not ON_DISK).
 */

/* ---- registry ----------------------------------------------------------- */

/* ---- registry ------------------------------------------------------------ */

void ps_reg_add(ps_store *s, ps_snapref *r)
{
    PS_ASSERT(s->reg_tail == NULL || s->reg_tail->lsn <= r->lsn);
    r->prev = s->reg_tail;
    r->next = NULL;
    if (s->reg_tail != NULL) {
        s->reg_tail->next = r;
    } else {
        s->reg_head = r;
    }
    s->reg_tail = r;
}

void ps_reg_remove(ps_store *s, ps_snapref *r)
{
    if (r->prev != NULL) {
        r->prev->next = r->next;
    } else {
        PS_ASSERT(s->reg_head == r);
        s->reg_head = r->next;
    }
    if (r->next != NULL) {
        r->next->prev = r->prev;
    } else {
        PS_ASSERT(s->reg_tail == r);
        s->reg_tail = r->prev;
    }
    r->prev = NULL;
    r->next = NULL;
}

u64 ps_reg_min(const ps_store *s)
{
    return s->reg_head != NULL ? s->reg_head->lsn : UINT64_MAX;
}

bool ps_reg_any_at_or_above(const ps_store *s, u64 c)
{
    return s->reg_tail != NULL && s->reg_tail->lsn >= c;
}

/* ---- failed state and log hooks ------------------------------------------- */

void ps_store_fail(ps_store *s, int err)
{
    const bool was = atomic_exchange_explicit(&s->failed, true, memory_order_acq_rel);
    if (!was) {
        ps_log(s, PS_LOG_ERROR, "store failed (error %d); reopen to recover", err);
    }
}

void ps_set_log_hooks(ps_store *s, const ps_log_hooks *hooks)
{
    pthread_mutex_lock(&s->ckpt_mu);
    if (hooks != NULL) {
        s->hooks = *hooks;
    } else {
        memset(&s->hooks, 0, sizeof(s->hooks));
    }
    pthread_mutex_unlock(&s->ckpt_mu);
}

int ps_checkpoint(ps_store *s)
{
    pthread_mutex_lock(&s->ckpt_mu);
    const int rv = ps_checkpoint_run(s);
    pthread_mutex_unlock(&s->ckpt_mu);
    return rv;
}

/* ---- checkpoint ---------------------------------------------------------- */

/*
 * Hold reg_mu. Commit released its pins on these old bitmap versions, so
 * the checkpoint was their last reader.
 */
static void free_bitmap_copies(ps_store *s)
{
    while (s->bmcopy_head != NULL) {
        ps_vnode *v = s->bmcopy_head;
        s->bmcopy_head = v->retire_next;
        v->retire_next = NULL;
        ps_entry *e = v->entry;
        const u32 id = e->id;
        ps_latch(s->pt, id);
        PS_ASSERT(e->head != v);
        ps_node_free(s, e, v);
        ps_unlatch(s->pt, id);
    }
}

/*
 * After a non-fatal failure (reg_mu held): moves every entry of the
 * checkpoint's list into next. The next bit is set before the old one
 * is cleared, so the entry never looks clean and is never released. An
 * entry already in next loses one membership.
 */
static void merge_back(ps_store *s)
{
    const u32 c = s->ckpt_idx;
    const u32 n = s->next_idx;
    const u8 cbit = (u8) (1u << c);
    const u8 nbit = (u8) (1u << n);
    ps_entry *e = s->dirty_head[c];
    while (e != NULL) {
        ps_entry *next = e->dirty_next[c];
        e->dirty_next[c] = NULL;
        const u8 was = atomic_fetch_or_explicit(&e->dirty, nbit, memory_order_relaxed);
        if ((was & nbit) != 0) {
            atomic_fetch_sub_explicit(&s->frames_dirty, 1, memory_order_relaxed);
        } else {
            e->dirty_next[n] = s->dirty_head[n];
            s->dirty_head[n] = e;
        }
        atomic_fetch_and_explicit(&e->dirty, (u8) ~cbit, memory_order_relaxed);
        e = next;
    }
    s->dirty_head[c] = NULL;
}

/*
 * began means ckpt_begin succeeded. Abort the backend only for a retryable
 * error; a fatal error leaves the backend failed.
 */
static int ckpt_failed(ps_store *s, u64 L, int err, bool fatal, bool began)
{
    if (began && !fatal) {
        s->be.ckpt_abort(s->be.ctx);
    }
    if (fatal) {
        ps_store_fail(s, err);
    }
    pthread_mutex_lock(&s->reg_mu);
    if (!fatal) {
        merge_back(s);
    }
    /* Retain the dirty list after a fatal failure for teardown at close. */
    free_bitmap_copies(s);
    ps_reg_remove(s, &s->ckpt_ref);
    s->ckpt_running = false;
    ps_retire_process(s);
    pthread_mutex_unlock(&s->reg_mu);
    atomic_fetch_add_explicit(&s->checkpoints_failed, 1, memory_order_relaxed);
    ps_log(
        s,
        fatal ? PS_LOG_ERROR : PS_LOG_WARN,
        "checkpoint at LSN %llu failed (error %d)%s",
        (unsigned long long) L,
        err,
        fatal ? "" : "; will retry");
    return fatal ? EIO : err;
}

static int item_cmp(const void *a, const void *b)
{
    const u32 x = ((const ps_ckpt_item *) a)->e->id;
    const u32 y = ((const ps_ckpt_item *) b)->e->id;
    return (x > y) - (x < y);
}

/* Copy the checkpoint's private dirty list into an array sorted by page ID. */
static int collect(ps_store *s, u32 c, u32 *count)
{
    u32 n = 0;
    for (const ps_entry *e = s->dirty_head[c]; e != NULL; e = e->dirty_next[c]) {
        n++;
    }
    if (n > s->ckpt_cap) {
        ps_ckpt_item *p = realloc(s->ckpt_items, (usize) n * sizeof(*p));
        if (p == NULL) {
            return ENOMEM;
        }
        s->ckpt_items = p;
        s->ckpt_cap = n;
        atomic_store_explicit(
            &s->ckpt_bytes,
            (u64) n * sizeof(*p) + s->page_size,
            memory_order_relaxed);
    }
    u32 i = 0;
    for (ps_entry *e = s->dirty_head[c]; e != NULL; e = e->dirty_next[c]) {
        s->ckpt_items[i].e = e;
        s->ckpt_items[i].v = NULL;
        s->ckpt_items[i].data = false;
        i++;
    }
    if (n > 1) {
        qsort(s->ckpt_items, n, sizeof(*s->ckpt_items), item_cmp);
    }
    *count = n;
    return 0;
}

/* Keep free markers for later cleanup, but count only data writes. */
static u32 find_visible(ps_store *s, u32 n, u64 L)
{
    u32 count = 0;
    for (u32 i = 0; i < n; i++) {
        ps_ckpt_item *it = &s->ckpt_items[i];
        const u32 id = it->e->id;
        ps_latch(s->pt, id);
        ps_vnode *v = ps_chain_visible(it->e, L);
        PS_ASSERT(v != NULL);
        PS_ASSERT(v->kind == PS_V_FREE || v->frame != PS_NO_FRAME);
        it->v = v;
        it->data = v->kind == PS_V_DATA;
        if (it->data) {
            count++;
        }
        ps_unlatch(s->pt, id);
    }
    return count;
}

/* Set checksums in the I/O buffer so the in-memory page bytes stay unchanged. */
static int write_pages(ps_store *s, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        const ps_ckpt_item *it = &s->ckpt_items[i];
        if (!it->data) {
            continue;
        }
        const u32 id = it->e->id;
        ps_latch(s->pt, id);
        memcpy(s->ckpt_buf, ps_node_page(s, it->v), s->page_size);
        ps_unlatch(s->pt, id);
        ps_page *p = (ps_page *) s->ckpt_buf;
        PS_ASSERT(p->page_id == id && p->checksum == 0);
        p->checksum = ps_crc32c(0, s->ckpt_buf, s->page_size);
        const int rv = s->be.ckpt_write(s->be.ctx, id, s->ckpt_buf);
        if (rv != 0) {
            return rv;
        }
    }
    return 0;
}

/*
 * Clear only this checkpoint's bit. A newer commit may have dirtied the
 * same entry on the other list.
 */
static void mark_written(ps_store *s, u32 n, u8 cbit)
{
    for (u32 i = 0; i < n; i++) {
        ps_entry *e = s->ckpt_items[i].e;
        ps_vnode *v = s->ckpt_items[i].v;
        const u32 id = e->id;
        ps_latch(s->pt, id);
        v->flags |= PS_VF_ON_DISK;
        const u8 was = atomic_fetch_and_explicit(&e->dirty, (u8) ~cbit, memory_order_relaxed);
        PS_ASSERT((was & cbit) != 0);
        PS_UNUSED(was);
        atomic_fetch_sub_explicit(&s->frames_dirty, 1, memory_order_relaxed);
        ps_drop_lone_marker(s, e);
        ps_update_evictable(s, e);
        ps_entry_maybe_release(s->pt, e);
        ps_unlatch(s->pt, id);
    }
}

int ps_checkpoint_run(ps_store *s)
{
    /* Capture the LSN and hand off the dirty list together under reg_mu. */
    pthread_mutex_lock(&s->reg_mu);
    if (atomic_load_explicit(&s->failed, memory_order_acquire)) {
        pthread_mutex_unlock(&s->reg_mu);
        return EIO;
    }
    const u64 L = atomic_load_explicit(&s->committed_lsn, memory_order_relaxed);
    const u32 c = s->next_idx;
    if (L == atomic_load_explicit(&s->durable_lsn, memory_order_relaxed) &&
        s->dirty_head[c] == NULL) {
        pthread_mutex_unlock(&s->reg_mu);
        return 0;
    }
    PS_ASSERT(!s->ckpt_running && s->dirty_head[c ^ 1] == NULL && s->bmcopy_head == NULL);
    const u32 num_groups = atomic_load_explicit(&s->num_groups, memory_order_relaxed);
    s->ckpt_ref.lsn = L;
    atomic_store_explicit(&s->ckpt_ref.dead, false, memory_order_relaxed);
    ps_reg_add(s, &s->ckpt_ref);
    s->ckpt_running = true;
    s->ckpt_lsn = L;
    s->ckpt_idx = c;
    s->next_idx = c ^ 1;
    pthread_mutex_unlock(&s->reg_mu);

    u32 n = 0;
    int rv = collect(s, c, &n);
    if (rv != 0) {
        return ckpt_failed(s, L, rv, false, false);
    }
    const u32 count = find_visible(s, n, L);

    /* The log must be durable through L before the page image is written. */
    if (s->hooks.flush_log != NULL) {
        rv = s->hooks.flush_log(s->hooks.ctx, L);
        if (rv != 0) {
            return ckpt_failed(s, L, rv, true, false);
        }
    }

    rv = s->be.ckpt_begin(s->be.ctx, L, num_groups, count);
    if (rv != 0) {
        return ckpt_failed(s, L, rv, false, false);
    }
    rv = write_pages(s, n);
    if (rv == 0) {
        bool fatal = false;
        rv = s->be.ckpt_commit(s->be.ctx, &fatal);
        if (rv != 0) {
            return ckpt_failed(s, L, rv, fatal, true);
        }
    }
    if (rv != 0) {
        return ckpt_failed(s, L, rv, false, true);
    }

    /* The bitmap is durable now, so skipped free markers count as on disk. */
    mark_written(s, n, (u8) (1u << c));

    pthread_mutex_lock(&s->reg_mu);
    atomic_store_explicit(&s->durable_lsn, L, memory_order_release);
    s->dirty_head[c] = NULL;
    free_bitmap_copies(s);
    ps_reg_remove(s, &s->ckpt_ref);
    s->ckpt_running = false;
    ps_retire_process(s);
    pthread_mutex_unlock(&s->reg_mu);
    atomic_fetch_add_explicit(&s->checkpoints_run, 1, memory_order_relaxed);
    ps_log(s, PS_LOG_DEBUG, "checkpoint at LSN %llu: %u pages", (unsigned long long) L, count);

    if (s->hooks.checkpoint_done != NULL) {
        s->hooks.checkpoint_done(s->hooks.ctx, L);
    }
    return 0;
}
