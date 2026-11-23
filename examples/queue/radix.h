#pragma once

/*
 * A radix array maps u64 indexes to fixed-width values stored in PageStore
 * pages. It suits indexes that grow at one end and are trimmed at the
 * other, such as message sequence numbers. Each index has a fixed place,
 * so nothing ever splits, merges or moves.
 *
 * A leaf holds leaf_cap values and an inner node holds fanout child page
 * IDs. A tree of height h covers the indexes below leaf_cap * fanout^(h-1)
 * and grows a new root as needed. With 4 KiB pages and 8-byte values, a
 * leaf holds 507 values, and three levels cover about 521 million indexes.
 *
 *     height 2             [ child 0 | child 1 | ... ]
 *                            /           \
 *     leaves       [ 0 .. 506 ]     [ 507 .. 1013 ]    ...
 *
 * Slots never set read as 0. An index with no leaf, never set or trimmed
 * away, reads as ENOENT.
 *
 * The array has no page of its own. The owner keeps root and height, for
 * example in its meta page, copies them into an rx_array before each call
 * and saves them after a change. Changes run inside the caller's store
 * operation. Abort the operation after a failed change.
 *
 * Example: append the next value at hi, then drop everything below new_lo.
 * lo is the bound from the previous trim.
 *
 *     rx_array a;
 *     rx_init(&a, ps_page_size(s), 8);
 *     a.root = saved_root;
 *     a.height = saved_height;
 *     int rv = rx_set(&a, s, hi, value);
 *     if (rv == 0) {
 *         rv = rx_trim(&a, s, lo, new_lo);
 *     }
 *     if (rv == 0) {
 *         save(a.root, a.height);
 *     }
 */

#include "pagestore/pagestore.h"

PS_EXTERN_C_BEGIN

/* Enough for every u64 index with any supported page size. */
#define RX_MAX_HEIGHT 8u

typedef struct rx_array {
    ps_page_id root; /* top node, PS_PAGE_ID_NONE while the array has no pages */
    u32 height; /* 0 when empty, 1 when the root is a leaf */
    u32 width; /* value bytes, 4 or 8 */
    u32 leaf_cap; /* values per leaf */
    u32 fanout; /* children per inner node */
} rx_array;

/* Filled in by rx_check. */
typedef struct rx_stats {
    u64 leaves;
    u64 inner;
} rx_stats;

/* Sets up an empty array. EINVAL for a width other than 4 or 8. */
int rx_init(rx_array *a, u32 page_size, u32 width);

/*
 * Stores value at index in the current operation and adds the pages that
 * hold it. EINVAL if the value doesn't fit the width.
 */
int rx_set(rx_array *a, ps_store *s, u64 index, u64 value);

/*
 * Reads through the current operation, so it sees the operation's own
 * changes. ENOENT if no leaf holds index.
 */
int rx_get_op(const rx_array *a, ps_store *s, u64 index, u64 *value);

/* Reads at the snapshot's LSN. ENOENT if no leaf holds index. */
int rx_get(const rx_array *a, ps_snapshot *snap, u64 index, u64 *value);

/*
 * Frees every node whose indexes all lie below to, in the current
 * operation. from must be the to of the previous trim, or 0 for the first.
 */
int rx_trim(rx_array *a, ps_store *s, u64 from, u64 to);

/*
 * Walks the whole tree and checks page types, levels and ranges. Every
 * index in [lo, hi) must have a leaf, and no node may lie entirely below
 * lo (a trim should have freed it) or at or above hi. A broken tree
 * returns EBADMSG and describes the first problem in msg.
 */
int rx_check(
    const rx_array *a,
    ps_snapshot *snap,
    u64 lo,
    u64 hi,
    rx_stats *out,
    char *msg,
    usize msg_cap);

PS_EXTERN_C_END
