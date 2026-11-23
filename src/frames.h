#pragma once

/*
 * The frame pool hands out page-sized slots in one aligned memory region.
 * Take the free-list mutex last. The owning page's latch guards changes
 * to a frame's owner and evictable flag. For eviction, read the owner ID,
 * latch that page, then check the ID again before using the owner pointer.
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
/* Clears ownership and CLOCK state so an evictor can reuse the allocated frame. */
void ps_frames_detach(ps_frames *fp, u32 f);
u32 ps_frames_free_count(const ps_frames *fp);

/* ps_frames_owner_id of a free, detached or not yet owned frame. */
#define PS_FRAMES_NO_OWNER PS_LITERAL(ps_page_id, UINT32_MAX)

void ps_frames_set_owner(ps_frames *fp, u32 f, struct ps_vnode *owner, ps_page_id id);
struct ps_vnode *ps_frames_owner(const ps_frames *fp, u32 f);
/* Page id of the owner, or PS_FRAMES_NO_OWNER. */
ps_page_id ps_frames_owner_id(const ps_frames *fp, u32 f);

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
