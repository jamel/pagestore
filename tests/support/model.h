#pragma once

/*
 * Reference model for allocation and page contents at any committed LSN.
 * LSN 0 represents a formatted store with group 0 and no user pages.
 *
 * Allocation follows src/allocator.h. Within an operation the cursor only
 * moves forward, even after cancelling an allocation. A committed page
 * freed in that operation becomes available at the next commit.
 *
 * The model assumes room for new groups and IDs. Use it from one thread.
 */

#include "pagestore/types.h"

typedef struct model model;

/* caller points into the model and is valid until the model changes. */
typedef struct model_page {
    const u8 *caller;
    u64 lsn;
} model_page;

model *model_create(u32 page_size);
void model_destroy(model *m);

u32 model_page_size(const model *m);
u32 model_caller_size(const model *m);
u32 model_group_size(const model *m);

/* Start at an LSN greater than model_last_lsn(m). */
void model_op_begin(model *m, u64 lsn);
/* The ID the store must return. */
u32 model_alloc(model *m);
/* id must be allocated in the operation's view (model_op_page). */
void model_free(model *m, u32 id);
/* off starts at the first caller byte, after the page header. */
void model_write(model *m, u32 id, u32 off, const void *data, u32 len);
void model_op_commit(model *m);
void model_op_abort(model *m);
bool model_in_op(const model *m);
/* Includes pending changes. An ID absent here is EINVAL for ps_read. */
bool model_op_page(const model *m, u32 id, model_page *out);
/* Predict allocations from the current cursor without advancing it. */
void model_op_next_ids(const model *m, u32 n, u32 *ids);

/* Pass NULL for out to check allocation alone. */
bool model_page_at(const model *m, u64 lsn, u32 id, model_page *out);
/*
 * Writes up to cap IDs in ascending order and returns the full count.
 * Pass NULL and 0 for a count alone.
 */
u32 model_allocated_at(const model *m, u64 lsn, u32 *ids, u32 cap);
/* Committed number of groups at the last LSN. */
u32 model_num_groups(const model *m);
/* True at the format LSN (0) and at each commit, even with gaps. */
bool model_has_lsn(const model *m, u64 lsn);
/* LSN of the last committed operation (0 if none). */
u64 model_last_lsn(const model *m);
/* Roll back to a recovery point after closing any open operation. */
void model_truncate(model *m, u64 lsn);
/* Predict allocations from the start of a fresh operation. */
void model_next_ids(const model *m, u32 n, u32 *ids);

/*
 * Pass ps_durable_lsn after each checkpoint so model_misuse_ok can
 * account for copies that reached disk. Repeating an LSN is fine.
 */
void model_durable(model *m, u64 lsn);

/*
 * Checks a read of an ID that is unallocated at the snapshot LSN. EINVAL
 * is always allowed and is the only result for reserved IDs. EBADMSG is
 * allowed until a checkpoint has written the page. A successful read may
 * return an old disk copy, even one a later checkpoint overwrote. Pass
 * the returned page's LSN and caller bytes.
 */
bool model_misuse_ok(const model *m, u64 lsn, u32 id, int rv, u64 page_lsn, const u8 *caller);
