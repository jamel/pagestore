#pragma once

/*
 * Store internals shared by store.c, op.c, pagein.c, versions.c, and mem.c.
 *
 * Lock order: the op gate (gate_mu; never held while an operation
 * runs), then ckpt_mu, then reg_mu, then one page latch at a time,
 * then leaf mutexes (frame free list, node allocator). reg_mu is never taken
 * while a page latch is held. Nobody waits on a condvar while holding reg_mu
 * or a page latch, except on the latch's own condvar for LOADING.
 */

#include "allocator.h"
#include "frames.h"
#include "pagestore/pagestore.h"
#include "ptable.h"

#include <pthread.h>
#include <stdatomic.h>

/*
 * Store-private ps_vnode.flags bits (ptable.h uses 0x01..0x08). Both are
 * guarded by the page latch and are only set while an operation runs.
 */
#define PS_VF_OP_PIN 0x10u /* holds one pin taken by the op's ps_read */
#define PS_VF_OP_BASE 0x20u /* lsn-0 disk node pushed by this op's ps_free */

/* Resources reserved for a group before the operation commits. */
typedef struct ps_op_group {
    u32 g;
    bool is_new; /* created by this operation */
    /* Bitmap entry. For a new group it is held (waiters + 1) until commit or abort. */
    ps_entry *e;
    /* Pinned head for an existing group, preallocated head for a new one. */
    ps_vnode *v;
    /* Frame for a bitmap copy or for the new bitmap itself. */
    u32 spare;
    /* Existing group: preallocated node for the copy. NULL for a new group. */
    ps_vnode *spare_node;
} ps_op_group;

/*
 * Snapshots and checkpoints join at the committed LSN, so insertion order
 * is also LSN order. reg_mu guards the links. Readers check dead atomically.
 */
typedef struct ps_snapref {
    u64 lsn;
    bool is_ckpt;
    atomic_bool dead; /* cut off */
    struct ps_snapref *prev;
    struct ps_snapref *next;
} ps_snapref;

/* A dirty entry paired with the version visible to this checkpoint. */
typedef struct ps_ckpt_item {
    ps_entry *e;
    ps_vnode *v;
    bool data; /* false for a free marker, kept only for bookkeeping */
} ps_ckpt_item;

/* Only the writer thread touches this state. */
typedef struct ps_op {
    u64 lsn;
    u32 frames; /* op_frames: copies, allocations, spares, distinct ps_read pins */

    /* Entries whose head is this operation's node (PS_VF_IN_OP); NULL = dropped. */
    ps_entry **pages;
    u32 npages;
    u32 pages_cap;

    /* Committed nodes pinned by ps_read (PS_VF_OP_PIN), one pin each. */
    ps_vnode **pins;
    u32 npins;
    u32 pins_cap;

    ps_op_group *groups;
    u32 ngroups;
    u32 groups_cap;
    u32 new_groups;

    /*
     * Entry and node reserved before the allocator runs, so after it
     * creates the group only the frame can still be missing.
     */
    ps_entry *kit_entry;
    ps_vnode *kit_node;
    u32 kit_group;
} ps_op;

struct ps_store {
    ps_config cfg; /* cfg.vfs is not kept (ps_open copies it into its backend) */
    ps_backend be;
    u32 page_size;
    u32 gsize; /* ids per group */
    u32 max_groups;
    u32 total_frames;
    u32 max_pinned; /* total_frames * max_pinned_pct / 100 */
    u32 op_limit; /* total_frames - max_pinned */

    ps_frames *frames;
    ps_ptable *pt;
    ps_allocator *alloc; /* writer only (and commit) */

    /* Read and write op_owner under gate_mu. */
    pthread_mutex_t gate_mu;
    pthread_cond_t gate_cv;
    atomic_bool op_active;
    pthread_t op_owner;

    /* Held for a whole checkpoint, so hooks and scratch belong to one attempt. */
    pthread_mutex_t ckpt_mu;
    ps_log_hooks hooks;
    ps_ckpt_item *ckpt_items; /* the running checkpoint's pages, sorted by id */
    u32 ckpt_cap;
    u8 *ckpt_buf; /* page-sized I/O buffer */

    /* Guards the registry, LSN publication, dirty lists and retired nodes. */
    pthread_mutex_t reg_mu;
    _Atomic u64 committed_lsn; /* written under reg_mu */
    _Atomic u64 durable_lsn; /* written under reg_mu */
    /*
     * Commits add entries to next_idx. A running checkpoint owns the other
     * list and can walk its links after releasing reg_mu.
     */
    ps_entry *dirty_head[2];
    u32 next_idx;
    u32 ckpt_idx;
    ps_snapref *reg_head; /* registry, oldest (lowest lsn) first */
    ps_snapref *reg_tail;
    ps_snapref ckpt_ref; /* the running checkpoint's registration */
    bool ckpt_running;
    u64 ckpt_lsn; /* LSN of the running checkpoint */
    /* Old bitmap versions held for the running checkpoint. */
    ps_vnode *bmcopy_head; /* through ps_vnode.retire_next */
    /* Retire queue, FIFO through ps_vnode.retire_next, ordered by retire_r. */
    ps_vnode *retire_head;
    ps_vnode *retire_tail;

    atomic_bool failed;

    ps_op op; /* writer only */

    /* Mirrors of the allocator's committed state, for ps_stats from any thread. */
    _Atomic u32 num_groups;
    _Atomic u64 allocated_pages;

    /* Counters. */
    _Atomic u32 frames_dirty; /* dirty-list memberships (each entry once per list) */
    _Atomic u32 frames_retired;
    _Atomic u32 frames_pinned; /* resident nodes with pins > 0 */
    _Atomic u64 cache_hits;
    _Atomic u64 cache_misses;
    _Atomic u64 op_bytes; /* capacity of the operation's arrays */
    _Atomic u64 ckpt_bytes; /* checkpoint array capacity and I/O buffer */
    _Atomic u64 checkpoints_run;
    _Atomic u64 checkpoints_failed;

    bool gate_init;
    bool ckpt_init;
    bool reg_init;
};

/* Sends a formatted message to the configured log callback, if any. */
void ps_log(ps_store *s, int level, const char *fmt, ...) PS_PRINTF(3, 4);

/* The first page ID in each group belongs to its bitmap. */
static inline bool ps_is_bitmap_id(const ps_store *s, u32 id)
{
    return id % s->gsize == 0;
}

static inline ps_page *ps_node_page(const ps_store *s, const ps_vnode *v)
{
    return (ps_page *) ps_frame(s->frames, v->frame);
}

/*
 * Frame requests (mem.c). Called with no page latch and without reg_mu.
 * Returns a free frame or fails with ENOBUFS.
 */
int ps_mem_frame(ps_store *s, bool writer, u32 *f);
void ps_mem_release(ps_store *s, u32 f);

/*
 * Page-in (pagein.c). Called with e's latch held, e not LOADING, and returns
 * with the latch held. Sets PS_E_LOADING and drops the latch for the frame
 * request and the read. frameless is a frameless data node of e to load
 * (kept alive by a temporary pin meanwhile), or NULL when e's chain is empty:
 * then a new committed ON_DISK head node (lsn from the page header) is
 * created. Checks that the header's page_id is e->id, that the bitmap flag
 * matches the id, and the CRC32C over the page with the checksum field
 * zeroed (EBADMSG); the in-memory checksum is left 0. Counts a cache miss.
 *
 * On success *out is the now-resident node with one pin taken for the
 * caller (release it with ps_node_unpin). On failure nothing is attached,
 * LOADING is cleared, waiters are woken and e may have been released (the
 * caller must not touch e; it still holds the latch of the id).
 */
int ps_page_in(ps_store *s, ps_entry *e, ps_vnode *frameless, bool writer, ps_vnode **out);

/* Version helpers (versions.c); e's latch held for all of them. */

/* Take one pin. Only the first pin on a resident node adds to frames_pinned. */
void ps_node_pin(ps_store *s, ps_vnode *v);

/*
 * Pinned nodes wait for their last unpin. Freeing the node can also
 * release e when nothing else needs the entry.
 */
void ps_node_free(ps_store *s, ps_entry *e, ps_vnode *v);

/*
 * A lone, unpinned ON_DISK free marker carries no information. If e's chain is just that and e is on no dirty list, the marker is
 * deleted (the chain becomes empty). The caller then calls
 * ps_entry_maybe_release. Every path that can leave such a marker alone
 * calls it: node free, checkpoint end, abort, and ps_free of an id this op
 * allocated.
 */
void ps_drop_lone_marker(ps_store *s, ps_entry *e);

/*
 * Drops one pin; at the last unpin a PS_VF_FREE_WHEN_UNPINNED node is
 * freed (ps_node_free, so e may be gone afterwards).
 */
void ps_node_unpin(ps_store *s, ps_entry *e, ps_vnode *v);

/*
 * The only code that sets or clears frames' evictable flags. A frame
 * is evictable iff its node is the chain head, committed (not IN_OP), a data
 * node, ON_DISK and unpinned, and e is neither a bitmap page nor LOADING.
 * Bitmap pages are never evicted: they stay resident from open (or their
 * creation) on, which lets the writer read committed bitmaps directly.
 */
void ps_update_evictable(ps_store *s, ps_entry *e);

/*
 * Hold reg_mu and e's latch. Dirty list links are preallocated in e.
 */
void ps_mark_dirty(ps_store *s, ps_entry *e);

/* ---- checkpoint, registry, retire queue (checkpoint.c) ---- */

/*
 * Enter with only ckpt_mu held. A retryable failure puts dirty entries
 * back on the next list. A fatal failure returns EIO and fails the store.
 */
int ps_checkpoint_run(ps_store *s);

/*
 * Puts the store in its failed state and logs err at ERROR.
 * Any thread; it calls the user's log callback, so it must not be called
 * with reg_mu or a page latch held (ckpt_mu is fine).
 */
void ps_store_fail(ps_store *s, int err);

/* Registry (reg_mu held). r->lsn must be >= every registered lsn. */
void ps_reg_add(ps_store *s, ps_snapref *r);
void ps_reg_remove(ps_store *s, ps_snapref *r);
/* Lowest registered lsn, UINT64_MAX if none. */
u64 ps_reg_min(const ps_store *s);
/* A version at c can only be visible if some reader has reached c. */
bool ps_reg_any_at_or_above(const ps_store *s, u64 c);

/*
 * Retire queue. ps_retire_push: reg_mu and v's latch held;
 * marks v RETIRED (counted in frames_retired while it has a frame) and
 * queues it with replacing LSN r, which must be >= every queued r.
 * ps_retire_process: reg_mu held, no latch; frees (ps_node_free, so pins
 * are respected) every queued node with r <= ps_reg_min.
 */
void ps_retire_push(ps_store *s, ps_vnode *v, u64 r);
void ps_retire_process(ps_store *s);

/* Format can start an operation at LSN 0 before the store is shared. */
void ps_op_begin_format(ps_store *s);

/* Test hooks. Quiescent store only. */
bool ps_test_has_entry(ps_store *s, u32 id);
u32 ps_test_pins(ps_store *s, u32 id); /* total pins on every version of id */
u32 ps_test_chain_len(ps_store *s, u32 id);
