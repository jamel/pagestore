#include "store.h"

#include "format.h"
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Store setup, teardown and statistics. Open formats a new store or loads
 * the committed bitmap pages from an existing one. User pages load on
 * demand, so open does not have to read the whole store.
 */

#define PS_DEFAULT_PAGE_SIZE 4096u
#define PS_MIN_FRAMES 16u

/* ---- configuration and logging ------------------------------------------- */

void ps_config_init(ps_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->page_size = 0;
    cfg->cache_size = (usize) 64 << 20;
    cfg->cache_memory = NULL;
    cfg->snapshot_pin_cap = 64;
    cfg->max_pinned_pct = 25;
    cfg->checkpoint_thread = true;
    cfg->checkpoint_interval_ms = 5000;
    cfg->checkpoint_dirty_pct = 40;
    cfg->log = NULL;
    cfg->log_ctx = NULL;
    cfg->vfs = NULL;
}

void ps_log(ps_store *s, int level, const char *fmt, ...)
{
    if (s->cfg.log == NULL) {
        return;
    }
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    s->cfg.log(s->cfg.log_ctx, level, msg);
}

/* These checks can run before opening the backend. */
static int check_config(const ps_config *cfg)
{
    if (cfg->page_size != 0 && !ps_page_size_valid(cfg->page_size)) {
        return EINVAL;
    }
    if (cfg->page_size != 0 && cfg->cache_size / cfg->page_size < PS_MIN_FRAMES) {
        return EINVAL;
    }
    if (cfg->snapshot_pin_cap == 0 || cfg->max_pinned_pct > 100 ||
        cfg->checkpoint_dirty_pct > 100) {
        return EINVAL;
    }
    return 0;
}

/* ---- open and close ------------------------------------------------------ */

/* Also used after a partial open. The caller closes the backend first. */
static void store_free(ps_store *s)
{
    ps_ptable_destroy(s->pt);
    ps_frames_destroy(s->frames);
    ps_allocator_destroy(s->alloc);
    free(s->op.pages);
    free(s->op.pins);
    free(s->op.groups);
    free(s->ckpt_items);
    free(s->ckpt_buf);
    if (s->reg_init) {
        pthread_mutex_destroy(&s->reg_mu);
    }
    if (s->ckpt_init) {
        pthread_mutex_destroy(&s->ckpt_mu);
    }
    if (s->gate_init) {
        pthread_cond_destroy(&s->gate_cv);
        pthread_mutex_destroy(&s->gate_mu);
    }
    free(s);
}

/* Record each completed lock setup so store_free can unwind a partial open. */
static int init_locks(ps_store *s)
{
    int rv = pthread_mutex_init(&s->gate_mu, NULL);
    if (rv != 0) {
        return rv;
    }
    rv = pthread_cond_init(&s->gate_cv, NULL);
    if (rv != 0) {
        pthread_mutex_destroy(&s->gate_mu);
        return rv;
    }
    s->gate_init = true;
    rv = pthread_mutex_init(&s->ckpt_mu, NULL);
    if (rv != 0) {
        return rv;
    }
    s->ckpt_init = true;
    rv = pthread_mutex_init(&s->reg_mu, NULL);
    if (rv != 0) {
        return rv;
    }
    s->reg_init = true;
    return 0;
}

/* Build the cache and allocator once the backend's page size is known. */
static int init_memory(ps_store *s, u32 page_size)
{
    s->page_size = page_size;
    s->gsize = ps_group_size(page_size);
    s->max_groups = ps_max_groups(page_size);
    int rv = ps_frames_create(page_size, s->cfg.cache_size, s->cfg.cache_memory, &s->frames);
    if (rv != 0) {
        return rv;
    }
    s->total_frames = ps_frames_count(s->frames);
    s->max_pinned = (u32) ((u64) s->total_frames * s->cfg.max_pinned_pct / 100);
    s->op_limit = s->total_frames - s->max_pinned;
    rv = ps_ptable_create(s->total_frames, &s->pt);
    if (rv != 0) {
        return rv;
    }
    s->ckpt_buf = malloc(page_size);
    if (s->ckpt_buf == NULL) {
        return ENOMEM;
    }
    atomic_store_explicit(&s->ckpt_bytes, page_size, memory_order_relaxed);
    return ps_allocator_create(page_size, &s->alloc);
}

/*
 * Reuse ps_alloc to create group 0. Freeing its first user page leaves just
 * the bitmap. The checkpoint advances the header from seq 0 to seq 1.
 */
static int format(ps_store *s)
{
    ps_op_begin_format(s);
    u32 id = 0;
    ps_page *p = NULL;
    int rv = ps_alloc(s, &id, &p);
    if (rv == 0) {
        PS_ASSERT(id == 1);
        rv = ps_free(s, id);
    }
    if (rv != 0) {
        ps_op_abort(s);
        return rv;
    }
    ps_op_commit(s);
    PS_ASSERT(ps_allocator_num_groups(s->alloc) == 1);
    return ps_checkpoint(s);
}

/* Register groups from the bitmap pages and keep their frames resident. */
static int load_groups(ps_store *s, u32 num_groups)
{
    for (u32 g = 0; g < num_groups; g++) {
        const u32 id = g * s->gsize;
        ps_latch(s->pt, id);
        ps_entry *e = NULL;
        int rv = ps_entry_get(s->pt, id, &e);
        ps_vnode *v = NULL;
        if (rv == 0) {
            e->flags |= PS_E_BITMAP;
            rv = ps_page_in(s, e, NULL, true, &v);
        }
        if (rv == 0) {
            const u8 *bits = (const u8 *) ps_node_page(s, v) + PS_PAGE_HEADER_SIZE;
            rv = ps_allocator_init_group(s->alloc, g, bits);
            if (rv == EINVAL) {
                ps_log(s, PS_LOG_ERROR, "bitmap page %u: its own bit is clear", id);
                rv = EBADMSG;
            }
            ps_node_unpin(s, e, v);
        }
        ps_unlatch(s->pt, id);
        if (rv != 0) {
            return rv;
        }
    }
    atomic_store_explicit(&s->num_groups, num_groups, memory_order_relaxed);
    atomic_store_explicit(
        &s->allocated_pages,
        ps_allocator_allocated(s->alloc),
        memory_order_relaxed);
    return 0;
}

int ps_open_backend(const ps_backend *be, const ps_config *cfg, u32 flags, ps_store **out)
{
    int rv = check_config(cfg);
    if (rv != 0) {
        be->close(be->ctx);
        return rv;
    }
    ps_store *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        be->close(be->ctx);
        return ENOMEM;
    }
    s->cfg = *cfg;
    s->cfg.vfs = NULL;
    s->be = *be;
    atomic_init(&s->op_active, false);
    atomic_init(&s->committed_lsn, 0);
    atomic_init(&s->durable_lsn, 0);
    atomic_init(&s->failed, false);
    atomic_init(&s->num_groups, 0);
    atomic_init(&s->allocated_pages, 0);
    atomic_init(&s->frames_dirty, 0);
    atomic_init(&s->frames_retired, 0);
    atomic_init(&s->frames_pinned, 0);
    atomic_init(&s->cache_hits, 0);
    atomic_init(&s->cache_misses, 0);
    atomic_init(&s->op_bytes, 0);
    atomic_init(&s->ckpt_bytes, 0);
    atomic_init(&s->checkpoints_run, 0);
    atomic_init(&s->checkpoints_failed, 0);
    atomic_init(&s->ckpt_ref.dead, false);
    s->ckpt_ref.is_ckpt = true;

    rv = init_locks(s);
    ps_backend_info info = {0};
    const bool create = (flags & PS_CREATE) != 0;
    if (rv == 0) {
        rv = s->be.open(s->be.ctx, create, cfg->page_size, &info);
    }
    if (rv == 0 && info.seq == 0 && !create) {
        rv = ENOENT; /* an interrupted format */
    }
    if (rv == 0 && cfg->page_size != 0 && cfg->page_size != info.page_size) {
        ps_log(
            s,
            PS_LOG_ERROR,
            "page size %u does not match the stored %u",
            cfg->page_size,
            info.page_size);
        rv = EINVAL;
    }
    if (rv == 0 && !ps_page_size_valid(info.page_size)) {
        rv = EBADMSG;
    }
    if (rv == 0 && info.seq != 0 &&
        (info.num_groups == 0 || info.num_groups > ps_max_groups(info.page_size))) {
        rv = EBADMSG;
    }
    if (rv == 0 && (u64) cfg->cache_size / info.page_size < PS_MIN_FRAMES) {
        rv = EINVAL;
    }
    if (rv == 0) {
        rv = init_memory(s, info.page_size);
    }
    if (rv == 0) {
        if (info.seq == 0) {
            rv = format(s);
        } else {
            atomic_store_explicit(&s->committed_lsn, info.durable_lsn, memory_order_relaxed);
            atomic_store_explicit(&s->durable_lsn, info.durable_lsn, memory_order_relaxed);
            rv = load_groups(s, info.num_groups);
        }
    }
    if (rv != 0) {
        s->be.close(s->be.ctx);
        store_free(s);
        return rv;
    }
    *out = s;
    return 0;
}

void ps_close(ps_store *s)
{
    if (s == NULL) {
        return;
    }
    PS_ASSERT(!atomic_load(&s->op_active));
    PS_ASSERT(!s->ckpt_running && s->reg_head == NULL);
    s->be.close(s->be.ctx);
    store_free(s);
}

/* ---- store state --------------------------------------------------------- */

u32 ps_page_size(const ps_store *s)
{
    return s->page_size;
}

u64 ps_committed_lsn(const ps_store *s)
{
    return atomic_load_explicit(&s->committed_lsn, memory_order_acquire);
}

u64 ps_durable_lsn(const ps_store *s)
{
    return atomic_load_explicit(&s->durable_lsn, memory_order_acquire);
}

void ps_stats(const ps_store *s, struct ps_stats *out)
{
    memset(out, 0, sizeof(*out));
    out->page_size = s->page_size;
    out->num_groups = atomic_load_explicit(&s->num_groups, memory_order_relaxed);
    out->allocated_pages = atomic_load_explicit(&s->allocated_pages, memory_order_relaxed);
    out->frames_total = s->total_frames;
    out->frames_free = ps_frames_free_count(s->frames);
    out->frames_evictable = ps_frames_evictable_count(s->frames);
    out->frames_dirty = atomic_load_explicit(&s->frames_dirty, memory_order_relaxed);
    out->frames_retired = atomic_load_explicit(&s->frames_retired, memory_order_relaxed);
    out->frames_pinned = atomic_load_explicit(&s->frames_pinned, memory_order_relaxed);
    out->bookkeeping_bytes = ps_ptable_bytes(s->pt) +
        atomic_load_explicit(&s->op_bytes, memory_order_relaxed) +
        atomic_load_explicit(&s->ckpt_bytes, memory_order_relaxed) + sizeof(*s);
    out->cache_hits = atomic_load_explicit(&s->cache_hits, memory_order_relaxed);
    out->cache_misses = atomic_load_explicit(&s->cache_misses, memory_order_relaxed);
    out->checkpoints_run = atomic_load_explicit(&s->checkpoints_run, memory_order_relaxed);
    out->checkpoints_failed = atomic_load_explicit(&s->checkpoints_failed, memory_order_relaxed);
    out->committed_lsn = ps_committed_lsn(s);
    out->durable_lsn = ps_durable_lsn(s);
    out->failed = atomic_load_explicit(&s->failed, memory_order_acquire);
}

/* ---- test hooks ---------------------------------------------------------- */

bool ps_test_has_entry(ps_store *s, u32 id)
{
    ps_latch(s->pt, id);
    const bool has = ps_entry_find(s->pt, id) != NULL;
    ps_unlatch(s->pt, id);
    return has;
}

u32 ps_test_pins(ps_store *s, u32 id)
{
    u32 pins = 0;
    ps_latch(s->pt, id);
    const ps_entry *e = ps_entry_find(s->pt, id);
    for (const ps_vnode *v = e != NULL ? e->head : NULL; v != NULL; v = v->older) {
        pins += v->pins;
    }
    ps_unlatch(s->pt, id);
    return pins;
}

u32 ps_test_chain_len(ps_store *s, u32 id)
{
    u32 n = 0;
    ps_latch(s->pt, id);
    const ps_entry *e = ps_entry_find(s->pt, id);
    for (const ps_vnode *v = e != NULL ? e->head : NULL; v != NULL; v = v->older) {
        n++;
    }
    ps_unlatch(s->pt, id);
    return n;
}
