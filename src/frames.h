#pragma once

/*
 * The frame pool hands out page-sized slots in one aligned memory region.
 * Frame pool: cache_size / page_size page-sized frames in one
 * 4096-aligned region, a free list, and per-frame CLOCK state.
 *
 * The free list is guarded by an internal leaf mutex. Each frame's
 * owner, evictable flag and reference bit, the counters and the clock hand
 * are atomics, so any thread may read them; owner and evictable are only
 * changed by the thread holding the owning page's latch.
 *
 * Frame indexes run from 0 to ps_frames_count() - 1; passing any other
 * index is an internal-invariant violation (asserted).
 * Valid frame indexes are [0, ps_frames_count()).
 */

#include "pagestore/types.h"

struct ps_vnode;

typedef struct ps_frames ps_frames;

/*
 * cache_size must fit at least 16 whole pages and fewer than UINT32_MAX.
 * A partial page is ignored. The pool borrows supplied memory, which must
 * be 4096-aligned and large enough for the whole frames. With NULL it
 * allocates and owns the buffer.
 */
int ps_frames_create(u32 page_size, usize cache_size, void *memory, ps_frames **out);
void ps_frames_destroy(ps_frames *fp);

u32 ps_frames_count(const ps_frames *fp);
void *ps_frame(const ps_frames *fp, u32 f);

/* ENOBUFS when the free list is empty. */
int ps_frames_alloc(ps_frames *fp, u32 *f);
/* Clears ownership and CLOCK state before returning the frame to the free list. */
void ps_frames_release(ps_frames *fp, u32 f);
u32 ps_frames_free_count(const ps_frames *fp);

void ps_frames_set_owner(ps_frames *fp, u32 f, struct ps_vnode *owner);
struct ps_vnode *ps_frames_owner(const ps_frames *fp, u32 f);

/* Setting the same value again leaves the evictable count alone. */
void ps_frames_set_evictable(ps_frames *fp, u32 f, bool on);
u32 ps_frames_evictable_count(const ps_frames *fp);

/* Give this frame a second chance on its next CLOCK visit. */
void ps_frames_touch(ps_frames *fp, u32 f);

/*
 * Returns a candidate with its reference bit set, so the next sweep gives
 * it a second chance even if eviction fails. Recheck under the page latch.
 * Returns ENOENT after 2 * ps_frames_count() probes without a candidate.
 */
int ps_frames_clock(ps_frames *fp, u32 *f);
