#pragma once

/*
 * Start with a store and model at the same LSN and enough cache space
 * to apply an operation. Generate it from the model.
 *
 *     w_op op;
 *     w_gen(&r, m, model_last_lsn(m) + 1, 4, &op);
 *     T_OK(w_apply_store(s, &op));
 *     w_apply_model(m, &op);
 *     w_check_store(s, m);
 *
 * Keep the seed to reproduce the workload. w_encode lets the same
 * operations go through WAL replay.
 */

#include "model.h"
#include "pagestore/pagestore.h"
#include "pagestore/types.h"
#include "pagestore/wal.h"
#include "rng.h"

typedef enum w_kind {
    W_ALLOC, /* id: the id the store must return */
    W_FREE,
    W_WRITE, /* off starts after the page header */
} w_kind;

typedef struct w_action {
    w_kind kind;
    ps_page_id id;
    u32 off;
    u32 len;
    u64 fill;
} w_action;

#define W_MAX_ACTIONS 16

typedef struct w_op {
    u64 lsn;
    u32 n;
    w_action a[W_MAX_ACTIONS];
} w_op;

/* Upper bound on ps_pressure.op_frames for generated operations. */
#define W_OP_FRAMES_MAX 7u

/* w_gen steers the number of allocated pages towards this. */
#define W_LIVE_TARGET 600u

/* Largest w_encode result. */
#define W_RECORD_MAX (20u + W_MAX_ACTIONS * 24u)

/* Byte off of the caller area written with fill. */
u8 w_pattern(u64 fill, u32 off);
/* Fill a slice using its offset in the full caller area. */
void w_pattern_fill(u64 fill, u32 off, u8 *dst, u32 len);

/*
 * Requires lsn above the last commit and max_actions in 1..W_MAX_ACTIONS.
 * If it finds no valid actions, it generates a single allocation.
 * It can rewrite an allocation in the same operation. Frees target
 * committed pages, which stay out of the draw for later actions.
 */
void w_gen(rng *r, const model *m, u64 lsn, u32 max_actions, w_op *out);

/*
 * Count the frames a valid operation could hold, including old versions
 * beneath copies and frees. The first allocation or free in a group
 * can need a spare bitmap frame.
 */
u32 w_op_frames_bound(const model *m, const w_op *op);

/* Returns the required size and leaves buf untouched when cap is too small. */
usize w_encode(const w_op *op, u8 *buf, usize cap);
/* Expects exactly one record, including its checksum. */
int w_decode(const void *buf, usize len, w_op *op);

/*
 * Checks allocated IDs against the generated operation. On success the
 * operation stays open, for example while the caller appends to the WAL.
 * On error it aborts any operation it started.
 */
int w_apply_store_begin(ps_store *s, const w_op *op);
/* w_apply_store_begin, then ps_op_commit. */
int w_apply_store(ps_store *s, const w_op *op);
/* Leave the operation open to inspect the writer view or test abort. */
void w_apply_model_begin(model *m, const w_op *op);
/* w_apply_model_begin, then model_op_commit. */
void w_apply_model(model *m, const w_op *op);

/*
 * Use as the ps_recover callback for w_encode records. EAGAIN triggers
 * a checkpoint and retry. Pass any callback context, including NULL.
 */
int w_apply_record(ps_apply_ctx *ctx, ps_store *s, u64 lsn, const void *rec, usize len);

/*
 * Check a healthy store with no operations in flight. A read can
 * trigger a checkpoint on EAGAIN and advance the durable LSN.
 */
void w_check_store(ps_store *s, const model *m);
