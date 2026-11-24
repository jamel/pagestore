#pragma once

/*
 * The page table keeps a newest-first version chain for each page ID.
 * Bucket and latch counts are powers of two, so every ID in a bucket shares
 * one latch.
 *
 * Take reg_mu before a page latch, hold at most one latch, and take the
 * node allocator's mutex last. Release the latch before I/O. Wait only on
 * the held latch's condition variable, and recheck after each wake.
 */

#include "pagestore/types.h"

/* ps_vnode.kind */
#define PS_V_DATA 0u
#define PS_V_FREE 1u

/* ps_vnode.flags */
#define PS_VF_ON_DISK 0x01u /* this version is the one in the data file */
#define PS_VF_IN_OP 0x02u /* produced by the in-progress operation */
#define PS_VF_RETIRED 0x04u /* on the retire queue */
#define PS_VF_FREE_WHEN_UNPINNED 0x08u /* freed at its last unpin */

/* ps_entry.flags */
#define PS_E_LOADING 0x01u /* a page-in is in flight; others wait on the latch */
#define PS_E_BITMAP 0x02u /* the page is an allocator bitmap page */

/* ps_vnode.frame of a node without a frame. */
#define PS_NO_FRAME UINT32_MAX

typedef struct ps_vnode {
    ps_lsn lsn; /* producing operation's LSN; PS_LSN_NONE for ps_free's unloaded disk node */
    struct ps_vnode *older; /* chain, newest first (latch); free-list link while unallocated */
    struct ps_entry *entry;
    u32 frame; /* PS_NO_FRAME when frameless */
    u32 pins; /* latch */
    u8 kind;
    u8 flags;
    struct ps_vnode *retire_next; /* retire queue or bitmap-copy list (reg_mu) */
    ps_lsn retire_r; /* replacing LSN while on the retire queue (reg_mu) */
} ps_vnode;

typedef struct ps_entry {
    ps_page_id id;
    u16 flags; /* PS_E_* (latch) */
    u16 waiters; /* threads waiting on LOADING (latch) */
    _Atomic u8 dirty; /* bit i = member of dirty list i */
    ps_vnode *head; /* newest version (latch) */
    struct ps_entry *hash_next; /* bucket chain (latch) */
    struct ps_entry *dirty_next[2]; /* dirty lists (reg_mu) */
} ps_entry;

typedef struct ps_ptable ps_ptable;

/* Uses at least two buckets per frame, rounded up to a power of two. */
int ps_ptable_create(u32 nframes, ps_ptable **out);
/* Frees table bookkeeping. Frames still belong to the frame pool. */
void ps_ptable_destroy(ps_ptable *pt);

u32 ps_ptable_nbuckets(const ps_ptable *pt);
u32 ps_ptable_nlatches(const ps_ptable *pt);

void ps_latch(ps_ptable *pt, ps_page_id id);
void ps_unlatch(ps_ptable *pt, ps_page_id id);
/* Hold the latch on entry. A broadcast for another page can wake this waiter. */
void ps_latch_wait(ps_ptable *pt, ps_page_id id);
/* The caller may broadcast after releasing the latch. */
void ps_latch_broadcast(ps_ptable *pt, ps_page_id id);

/* Hold the page latch. NULL if id has no entry. */
ps_entry *ps_entry_find(ps_ptable *pt, ps_page_id id);
/* Hold the page latch. Finds the entry or allocates an empty one. */
int ps_entry_get(ps_ptable *pt, ps_page_id id, ps_entry **out);
/* Hold the page latch. Loading, waiters and dirty lists can retain an empty entry. */
void ps_entry_maybe_release(ps_ptable *pt, ps_entry *e);

/* Returns a new data node with frame = PS_NO_FRAME. Any thread may allocate. */
int ps_vnode_new(ps_ptable *pt, ps_vnode **out);
/* Slabs stay allocated until the table is destroyed. */
void ps_vnode_delete(ps_ptable *pt, ps_vnode *v);

/* Hold the page latch. Returns the newest node at or below lsn, which may mark a free page. */
ps_vnode *ps_chain_visible(const ps_entry *e, ps_lsn lsn);

/* Counts buckets, live entries and slabs. Safe to sample from any thread. */
u64 ps_ptable_bytes(const ps_ptable *pt);
