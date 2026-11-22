#pragma once

/*
 * The allocator tracks page IDs in grouped bitmaps for the writer. Group g
 * starts at page g * ps_group_size(page_size), which holds its bitmap. Bits
 * at and beyond UINT32_MAX are marked occupied.
 *
 * Each operation takes the lowest committed free ID above its last
 * allocation, so a page freed in an operation is reused only by a later one.
 * To commit, apply each group from ps_allocator_op_groups with
 * ps_allocator_apply, then call ps_allocator_op_commit. These steps need no
 * allocation or I/O.
 */

#include "pagestore/types.h"

typedef struct ps_allocator ps_allocator;

typedef struct ps_bitmap_get_ctx ps_bitmap_get_ctx;

/*
 * Return the committed bitmap payload in *bits and keep it valid and
 * unchanged until the operation ends. The allocator fetches only groups
 * that existed at the start and reuses successful fetches. Errors pass through.
 */
typedef int (*ps_bitmap_get_fn)(ps_bitmap_get_ctx *ctx, u32 group, const u8 **bits);

/* The bitmap starts after the 24-byte page header. */
u32 ps_group_size(u32 page_size);
/* Includes the partial last group. */
u32 ps_max_groups(u32 page_size);

/* The allocator starts empty. Register committed groups before allocating. */
int ps_allocator_create(u32 page_size, ps_allocator **out);
void ps_allocator_destroy(ps_allocator *a);

/*
 * At open, register groups in order starting at 0. Bit 0 must be set.
 * Bits beyond the valid ID range are ignored.
 */
int ps_allocator_init_group(ps_allocator *a, u32 g, const u8 *bits);

u32 ps_allocator_num_groups(const ps_allocator *a);
/* Counts allocated user pages in the committed state. */
u64 ps_allocator_allocated(const ps_allocator *a);

void ps_allocator_op_begin(ps_allocator *a);

/*
 * Allocates the next id (see the contract above). *new_group is true if this
 * call created a group (its bitmap page is created at commit). ENOSPC when
 * every possible group is full; ENOMEM; or get's error. On failure nothing
 * changes.
 */
int ps_allocator_next(
    ps_allocator *a,
    ps_bitmap_get_fn get,
    ps_bitmap_get_ctx *ctx,
    u32 *id,
    bool *new_group);

/*
 * *cancelled is true when this operation allocated id. Other frees become
 * reusable after commit. EINVAL covers reserved IDs and pages already free.
 */
int ps_allocator_free(
    ps_allocator *a,
    ps_bitmap_get_fn get,
    ps_bitmap_get_ctx *ctx,
    u32 id,
    bool *cancelled);

/*
 * Includes new groups and groups with cancelled allocations, in ascending
 * order. The array is valid until the next allocation or free, or the end of
 * the operation.
 */
u32 ps_allocator_op_groups(const ps_allocator *a, const u32 **groups);
bool ps_allocator_group_is_new(const ps_allocator *a, u32 g);

/*
 * Pass the current bitmap payload for a group from ps_allocator_op_groups.
 * Zero a new group's payload first. Apply sets its reserved bits.
 */
void ps_allocator_apply(const ps_allocator *a, u32 g, u8 *bits);

/* Makes the operation's changes the committed state. */
void ps_allocator_op_commit(ps_allocator *a);
/* Discard pending changes and keep the group buffers for the next operation. */
void ps_allocator_op_abort(ps_allocator *a);
