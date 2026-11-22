#pragma once

/*
 * Shared store state. The writer owns the operation and allocator state.
 * reg_mu guards commit publication and the reader registry. A page latch
 * guards its version chain.
 *
 * Lock order: gate_mu, ckpt_mu, reg_mu, one page latch, then the frame
 * free list, node allocator and mem_mu last. Release gate_mu before running
 * the operation. Wait for LOADING holding only the page latch, and for
 * checkpoints or frames holding only mem_mu.
 */

#include "allocator.h"
#include "frames.h"
#include "pagestore/pagestore.h"
#include "ptable.h"

#include <pthread.h>
#include <stdatomic.h>

/*
 * The page latch guards these operation-only bits in ps_vnode.flags.
 */
#define PS_VF_OP_PIN 0x10u /* holds one pin taken by the operation's ps_read */
#define PS_VF_OP_BASE 0x20u /* disk node added by this operation's ps_free */
/*
 * The operation made this committed frame unevictable. It counts in
 * op_frames only if PS_VF_OP_PIN is clear.
 */
#define PS_VF_OP_HELD 0x40u

/*
 * Minimum operation frame budget for the store to open: a page, its copy,
 * a new group's bitmap and a spare.
 */
#define PS_OP_MIN_FRAMES 4u

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
    /* Set under reg_mu before unregistering a cut-off snapshot. */
    atomic_bool dead;
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
    u32 frames; /* op_frames in ps_pressure */
    u32 reserved; /* frames the current call will add after its allocator call */

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
    ps_config cfg; /* See vfs for the store's VFS copy. */
    ps_backend be;
    /* The WAL uses the same VFS as the store. */
    ps_vfs vfs;
    char *dir; /* owned path from ps_open, NULL for ps_open_backend */
    u32 page_size;
    u32 gsize; /* ids per group */
    u32 max_groups;
    u32 total_frames;
    u32 max_pinned; /* snapshot pin budget, in frames */

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

    /* mem_mu guards the checkpoint attempt counters and wait conditions. */
    pthread_mutex_t mem_mu;
    pthread_cond_t ckpt_thread_cv; /* the thread sleeps here */
    pthread_cond_t ckpt_wait_cv; /* frame-request waiters sleep here */
    u64 ckpt_started; /* attempts started */
    u64 ckpt_done; /* attempts completed */
    bool ckpt_inflight;
    int ckpt_last_err; /* result of the last completed attempt */
    u32 ckpt_waiters;
    bool ckpt_want; /* a request or a waiter asked for an attempt */
    bool ckpt_stop; /* ps_close: the thread exits */
    u64 ckpt_last_end_ms; /* monotonic end time of the last attempt */
    u32 ckpt_backoff_ms; /* reset after a success */
    u64 ckpt_retry_at_ms; /* the thread starts no attempt before this */
    /* Written only by open (before the store is shared) and ps_close (after the join). */
    bool ckpt_thread_started;
    pthread_t ckpt_thread;
    bool ckpt_bg; /* ckpt_mu: the running attempt is the thread's */

    ps_op op; /* writer only */

    /* Mirrors of the allocator's committed state, for ps_stats from any thread. */
    _Atomic u32 num_groups;
    _Atomic u64 allocated_pages;

    /* Each dirty entry counts once, free markers included. */
    _Atomic u32 frames_dirty;
    _Atomic u32 frames_retired;
    _Atomic u32 frames_pinned; /* resident nodes with pins > 0 */
    _Atomic u64 cache_hits;
    _Atomic u64 cache_misses;
    _Atomic u64 op_bytes; /* capacity of the operation's arrays */
    _Atomic u64 ckpt_bytes; /* checkpoint array capacity and I/O buffer */
    _Atomic u64 checkpoints_run;
    _Atomic u64 checkpoints_failed;
    _Atomic u32 snap_pins; /* pins held by all snapshots, at most max_pinned */
    _Atomic u32 snapshots_open; /* open handles, cut-off ones included */
    _Atomic u64 snapshots_cut_off;
    _Atomic u64 snap_bytes; /* open snapshot handles */
    _Atomic u64 writer_waits; /* frame requests that waited for the checkpointer */

    bool gate_init;
    bool ckpt_init;
    bool reg_init;
    bool mem_init;
};

/*
 * The WAL borrows the path and VFS until ps_close. A custom backend has
 * no path, so ps_store_dir returns NULL for it.
 */
const char *ps_store_dir(const ps_store *s);
const ps_vfs *ps_store_vfs(const ps_store *s);

/* Sends a formatted message to the configured log callback, if any. */
void ps_log(ps_store *s, int level, const char *fmt, ...) PS_PRINTF(3, 4);

/* Thread-safe strerror: the message for err, in buf (n bytes) or a static string. */
const char *ps_strerror(int err, char *buf, usize n);

/* The first page ID in each group belongs to its bitmap. */
static inline bool ps_is_bitmap_id(const ps_store *s, u32 id)
{
    return id % s->gsize == 0;
}

/*
 * Frames left after the snapshot budget and the resident bitmaps. A new
 * group's bitmap is charged to the operation until commit.
 */
static inline u32 ps_op_limit(const ps_store *s)
{
    const u64 reserved =
        (u64) s->max_pinned + atomic_load_explicit(&s->num_groups, memory_order_relaxed);
    return reserved < s->total_frames ? s->total_frames - (u32) reserved : 0;
}

/* The caller must keep the node's frame resident while using the pointer. */
static inline ps_page *ps_node_page(const ps_store *s, const ps_vnode *v)
{
    return (ps_page *) ps_frame(s->frames, v->frame);
}

/*
 * Call without a page latch or reg_mu, after checking the operation's
 * budget. May wait for a checkpoint, and a writer may cut off snapshots.
 * EAGAIN asks for a manual checkpoint. ENOBUFS means no frame can be freed.
 */
int ps_mem_frame(ps_store *s, bool writer, u32 *f);
void ps_mem_release(ps_store *s, u32 f);

/*
 * Hold e's latch. On success, *f is still allocated and e may be gone.
 * A head with older versions keeps its LSN in a frameless node.
 */
bool ps_evict_head(ps_store *s, ps_entry *e, u32 *f);

/*
 * Enter with e latched and LOADING clear. Pass a frameless data node, or
 * NULL for an empty chain. Drops the latch for I/O and returns with it held.
 * On success *out owns one pin. On failure e may be gone, so use only the
 * page ID.
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
 * Drop a lone disk node or free marker once the durable image can stand
 * in for it. The caller can then try ps_entry_maybe_release.
 */
void ps_drop_lone_head(ps_store *s, ps_entry *e);

/*
 * The last unpin can free v and release e. It can also drop a clean frame
 * a snapshot loaded below an in-op free marker, where CLOCK cannot reach it.
 */
void ps_node_unpin(ps_store *s, ps_entry *e, ps_vnode *v);

/*
 * Only a clean, unpinned data head can be a CLOCK candidate. Bitmaps stay
 * resident so the allocator can read the committed bits directly.
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
 * Wakes frame waiters with EIO and stops further checkpoints. Takes mem_mu
 * and calls the log callback. Keep reg_mu, page latches and mem_mu free.
 */
void ps_store_fail(ps_store *s, int err);

/* ---- checkpoint thread and waits (checkpoint.c) ---- */

/* Set up mem_mu and the two condition variables used by the checkpointer. */
int ps_ckpt_sync_init(ps_store *s);
void ps_ckpt_sync_destroy(ps_store *s);

/*
 * Start as the last step of open. Stop joins the thread after its current
 * checkpoint finishes and is safe when the thread was never started.
 */
int ps_ckpt_thread_start(ps_store *s);
void ps_ckpt_thread_stop(ps_store *s);

/*
 * Call with no locks held. Waits for a running or newly requested attempt,
 * or returns the last error after a failed attempt. EIO if the store has
 * failed or is closing.
 * A successful attempt can leave the caller without a usable frame.
 */
int ps_ckpt_wait(ps_store *s);

/* Call after commit releases reg_mu. */
void ps_ckpt_note_commit(ps_store *s);

/* Registry (reg_mu held). r->lsn must be >= every registered lsn. */
void ps_reg_add(ps_store *s, ps_snapref *r);
void ps_reg_remove(ps_store *s, ps_snapref *r);
/* Lowest registered lsn, UINT64_MAX if none. */
u64 ps_reg_min(const ps_store *s);
/* A version at c can only be visible if some reader has reached c. */
bool ps_reg_any_at_or_above(const ps_store *s, u64 c);

/* ---- snapshots, retire queue, cut-off (snapshot.c) ---- */

/*
 * Hold reg_mu for both. Push also needs v's latch; r is the replacing LSN.
 * Process takes page latches itself and drops versions once every
 * registered reader has reached r. Pinned versions live until unpin.
 */
void ps_retire_push(ps_store *s, ps_vnode *v, u64 r);
void ps_retire_process(ps_store *s);

/*
 * Call without reg_mu or a page latch. Returns false if no snapshot can be
 * cut off. A cut-off snapshot keeps its pins but new reads return ESTALE.
 */
bool ps_snap_cut_off_oldest(ps_store *s);

/*
 * Cut off a snapshot only if doing so can help free a frame. The running
 * checkpoint still protects versions visible at its LSN.
 */
bool ps_snap_cut_off_for_frames(ps_store *s);

/* Format can start an operation at LSN 0 before the store is shared. */
void ps_op_begin_format(ps_store *s);

/* Recovery uses this to check whether its callback left an operation open. */
bool ps_op_active_here(ps_store *s);

/* Test hooks. Quiescent store only. */
bool ps_test_has_entry(ps_store *s, u32 id);
u32 ps_test_pins(ps_store *s, u32 id); /* total pins on every version of id */
u32 ps_test_chain_len(ps_store *s, u32 id);
/* Threads waiting on id's LOADING (or holding its entry). Any time. */
u32 ps_test_waiters(ps_store *s, u32 id);
/*
 * Evicts id's head with the evictor (ps_evict_head) and frees its frame.
 * Returns whether it did.
 */
bool ps_test_evict(ps_store *s, u32 id);
/* Frame requests waiting in ps_ckpt_wait. Any time. */
u32 ps_test_ckpt_waiters(ps_store *s);
/* True once ps_close has asked the checkpoint thread to stop. Any time. */
bool ps_test_ckpt_stopping(ps_store *s);
