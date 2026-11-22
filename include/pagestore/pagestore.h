#pragma once

/*
 * PageStore manages fixed-size pages. One writer groups changes into an
 * operation and publishes them together with ps_op_commit. Commits stay in
 * memory until ps_checkpoint makes them durable.
 *
 * Functions returning int use 0 for success and a positive errno on failure.
 * Detected API misuse returns EINVAL and logs at PS_LOG_ERROR in every build.
 */

#include "pagestore/backend.h"
#include "pagestore/compiler.h"
#include "pagestore/types.h"
#include "pagestore/vfs.h"

PS_EXTERN_C_BEGIN

typedef struct ps_store ps_store;
typedef struct ps_snapshot ps_snapshot;

/* Every page starts with this 24-byte header. The rest belongs to the caller. */
typedef struct ps_page {
    u32 magic; /* caller's page type */
    u32 page_id; /* store-owned */
    u32 checksum; /* CRC32C on disk, always 0 in memory */
    u32 flags; /* store-owned, see PS_PAGE_FLAG_* */
    u64 lsn; /* store-owned, set by the operation that produced this version */
} ps_page;

#define PS_PAGE_HEADER_SIZE 24
PS_STATIC_ASSERT(sizeof(ps_page) == PS_PAGE_HEADER_SIZE, "ps_page must be 24 bytes");

/* Set in ps_page.flags on allocator bitmap pages. */
#define PS_PAGE_FLAG_BITMAP 0x1u
/* ps_page.magic of a bitmap page ("BMSP" little-endian). */
#define PS_BITMAP_MAGIC 0x50534D42u

/* Log levels passed to ps_config.log. */
#define PS_LOG_ERROR 0
#define PS_LOG_WARN 1
#define PS_LOG_INFO 2
#define PS_LOG_DEBUG 3

/* Define struct ps_log_ctx to pass state to the log callback. */
typedef struct ps_log_ctx ps_log_ctx;

/* Receives one formatted log line (no trailing newline). May be called from any thread. */
typedef void (*ps_log_fn)(ps_log_ctx *ctx, int level, const char *msg);

typedef struct ps_config {
    u32 page_size; /* 0 uses 4096 on create or the stored size on open */
    usize cache_size; /* bytes of frame memory, at least 16 frames */
    void *cache_memory; /* optional caller-owned buffer, cache_size bytes, aligned to 4096 */
    u32 snapshot_pin_cap; /* pins per snapshot */
    u32 max_pinned_pct; /* share of the cache all snapshots may pin, 1..100 */
    /*
     * The thread wakes on a request, a frame wait, the dirty watermark or
     * the timer. It logs failures at PS_LOG_ERROR and starts retrying after
     * 100 ms, doubling the delay up to 10 s. With the thread disabled, a
     * frame request that needs a checkpoint returns EAGAIN.
     */
    bool checkpoint_thread;
    u32 checkpoint_interval_ms; /* 0 = no timer */
    u32 checkpoint_dirty_pct; /* dirty watermark, 0..100 */
    ps_log_fn log; /* optional */
    ps_log_ctx *log_ctx;
    const ps_vfs *vfs; /* ps_open only; NULL = POSIX. Copied at open. */
} ps_config;

/*
 * Defaults to an internally allocated 64 MiB cache, 64 pins per snapshot and
 * a 25% total pin budget. The checkpoint thread runs on a 5 s timer and at
 * a 40% dirty watermark. Uses the default page size and POSIX VFS with logging off.
 */
void ps_config_init(ps_config *cfg);

#define PS_CREATE 0x1u

/* Opens (PS_CREATE: creating or formatting if needed) the store in dir. */
int ps_open(const char *dir, const ps_config *cfg, u32 flags, ps_store **out);

/*
 * Takes ownership of the backend, even on failure: a failed open calls
 * be->close, and otherwise ps_close does. Errors are as for ps_open, and
 * backend errors pass through.
 */
int ps_open_backend(const ps_backend *be, const ps_config *cfg, u32 flags, ps_store **out);

/*
 * Close snapshots and end the operation first. Waits for the background
 * checkpointer. Does not checkpoint, so call ps_checkpoint first to make
 * recent commits durable.
 */
void ps_close(ps_store *s);

u32 ps_page_size(const ps_store *s);
/* The committed LSN can be ahead of the last durable checkpoint. */
u64 ps_committed_lsn(const ps_store *s);
u64 ps_durable_lsn(const ps_store *s);

/*
 * op_frames includes copies, allocations, distinct ps_read pins and a
 * spare per group touched or created. A copy also holds its committed
 * version in memory. Reading a committed page and then writing it costs
 * two frames in total. op_limit leaves room for the snapshot budget
 * and one resident bitmap per group. A call beyond the limit returns ENOBUFS.
 *
 * The cache must leave at least four frames for an operation. Open returns
 * EINVAL below that floor. When a full store would cross it by adding a
 * group, ps_alloc logs at PS_LOG_ERROR and returns ENOBUFS. Reopen with a
 * larger cache to add groups.
 */
typedef struct ps_pressure {
    u32 op_frames; /* frames held by the current operation */
    u32 op_limit; /* most frames an operation may hold */
    /* Counts each dirty page ID once. Free markers count without a frame. */
    u32 dirty_frames;
    u32 total_frames;
} ps_pressure;

/*
 * Choose a log sequence number (LSN) higher than the last committed one.
 * If another thread owns the operation, begin waits before checking lsn.
 * An old LSN or a nested begin returns EINVAL. A failed store returns EIO.
 */
int ps_op_begin(ps_store *s, u64 lsn);
/* Publishes the operation in memory without allocating or doing I/O. */
void ps_op_commit(ps_store *s);
/* Discards every change of the operation. */
void ps_op_abort(ps_store *s);
/* On the operation's thread (op_frames is 0 outside an operation). */
void ps_op_pressure(const ps_store *s, ps_pressure *out);

/*
 * Call on the operation's thread, and abort after an error. Edit the copy
 * that ps_write returns. Earlier ps_read pointers keep the old bytes.
 * Pointers expire at commit or abort, or sooner when ps_free discards a
 * page allocated or copied in this operation.
 */
int ps_alloc(ps_store *s, u32 *id, ps_page **page);
int ps_free(ps_store *s, u32 id);
int ps_read(ps_store *s, u32 id, const ps_page **page);
int ps_write(ps_store *s, u32 id, ps_page **page);

/*
 * A snapshot sees the committed pages as they were when it opened. Use each
 * handle from one thread at a time. On a failed store, open returns EIO and
 * existing snapshots can read only resident pages.
 */
int ps_snapshot_open(ps_store *s, ps_snapshot **out);
u64 ps_snapshot_lsn(const ps_snapshot *snap);
/*
 * Pins the page until unpin or close. Reading a pinned ID again returns the
 * same pointer without a second pin. ENOBUFS when pins or frames run out.
 * ESTALE when the writer cut off the snapshot. Pins already held stay valid.
 */
int ps_snapshot_read(ps_snapshot *snap, u32 id, const ps_page **page);
void ps_snapshot_unpin(ps_snapshot *snap, u32 id);
void ps_snapshot_close(ps_snapshot *snap);

/*
 * Makes the pages as of the current committed LSN durable, on this thread.
 * A fatal failure returns EIO and fails the store. After a nonfatal error
 * such as ENOSPC, a later checkpoint can retry.
 */
int ps_checkpoint(ps_store *s);
/*
 * Wakes the background checkpointer and returns at once. Any thread. No
 * effect without the checkpoint thread or on a failed store.
 */
void ps_checkpoint_request(ps_store *s);

/* Define struct ps_log_hooks_ctx to pass state to the hooks. */
typedef struct ps_log_hooks_ctx ps_log_hooks_ctx;

/*
 * Runs before checkpoint page writes. Return 0 once the log is durable
 * through lsn. Any other return fails the store.
 */
typedef int (*ps_flush_log_fn)(ps_log_hooks_ctx *ctx, u64 lsn);
/* Runs after the checkpoint is durable. Log records through lsn can be dropped. */
typedef void (*ps_checkpoint_done_fn)(ps_log_hooks_ctx *ctx, u64 lsn);

/* Either log hook may be NULL. */
typedef struct ps_log_hooks {
    ps_flush_log_fn flush_log;
    ps_checkpoint_done_fn checkpoint_done;
    ps_log_hooks_ctx *ctx;
} ps_log_hooks;

/*
 * Copies *hooks, or clears them for NULL. Any thread. Waits for a running
 * checkpoint. Hooks run with the checkpoint lock held. They must not call
 * ps_checkpoint or ps_set_log_hooks, or start operations or snapshot reads
 * that could wait for the writer or for frames. They may use the WAL.
 */
void ps_set_log_hooks(ps_store *s, const ps_log_hooks *hooks);

/* The struct tag leaves the name ps_stats available to the C function. */
struct ps_stats {
    u32 page_size;
    u32 num_groups;
    u64 allocated_pages;
    u32 frames_total;
    u32 frames_free;
    u32 frames_evictable;
    u32 frames_dirty;
    u32 frames_retired;
    u32 frames_pinned;
    u64 bookkeeping_bytes;
    u64 cache_hits;
    u64 cache_misses;
    u32 snapshots_open;
    u64 snapshots_cut_off;
    u64 checkpoints_run;
    u64 checkpoints_failed;
    u64 writer_waits; /* frame requests (writer or reader) that waited for the checkpointer */
    u64 committed_lsn;
    u64 durable_lsn;
    bool failed;
};

/* Any thread can call this. Counts are exact when other threads are idle. */
void ps_stats(const ps_store *s, struct ps_stats *out);

PS_EXTERN_C_END
