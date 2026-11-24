#pragma once

/*
 * The write-ahead log (WAL) stores caller-defined records in LSN order.
 * Open the store, then its WAL, then call ps_recover before writing.
 * Append and flush each operation's record before committing it.
 *
 *     int rv = ps_wal_append(wal, lsn, record, len);
 *     if (rv == 0) {
 *         rv = ps_wal_flush(wal, lsn);
 *     }
 *     if (rv != 0) {
 *         ps_op_abort(store);
 *         return rv;
 *     }
 *     ps_op_commit(store);
 *
 * A record can survive a flush error and be replayed at the next open.
 */

#include "pagestore/types.h" // IWYU pragma: export

PS_EXTERN_C_BEGIN

typedef struct ps_wal ps_wal;
typedef struct ps_wal_iter ps_wal_iter;

typedef struct ps_wal_config {
    u64 segment_size; /* preallocated file size, minimum 64 KiB */
    usize buffer_size; /* 0 writes records directly */
    u32 max_record_size; /* payload limit, at most segment_size - 56 before padding */
} ps_wal_config;

/* Defaults to 64 MiB segments, a 1 MiB buffer, and a 16 MiB record limit. */
void ps_wal_config_init(ps_wal_config *cfg);

/*
 * Call at startup with no operation in progress. waldir is created if its
 * parent exists. NULL waldir means <dir>/wal and works only for ps_open
 * stores. NULL cfg uses the defaults. Deletes segments the store no longer
 * needs and cuts off an incomplete tail. EBADMSG if a needed segment is
 * missing or damaged, or if the log is empty but the durable LSN is not 0.
 * EINVAL for bad arguments, EBUSY if locked. Installs the store's log hooks.
 */
int ps_wal_open(ps_store *store, const char *waldir, const ps_wal_config *cfg, ps_wal **out);

/*
 * Writer only. EINVAL if lsn does not exceed the last LSN in the log or len
 * exceeds max_record_size. ENOMEM, and ENOSPC or EDQUOT when starting a new
 * segment, leave the record unwritten and can be retried. After a fatal
 * error, append and flush return EIO until reopen.
 */
int ps_wal_append(ps_wal *w, ps_lsn lsn, const void *buf, usize len);

/*
 * Makes records through lsn durable, up to the last appended LSN. Any
 * thread. A write or sync failure is fatal: flushes and appends return EIO,
 * and store checkpoints that need the WAL fail.
 */
int ps_wal_flush(ps_wal *w, ps_lsn lsn);

/*
 * Drops segments that end at or below lsn, but keeps the newest. Any thread.
 * While iterators are open, deletion waits for a later call. An unlink error
 * is returned without failing the WAL.
 */
int ps_wal_truncate(ps_wal *w, ps_lsn lsn);

/*
 * Starts at the first retained record with LSN >= from_lsn. One thread per
 * iterator. Iterators work only before the first append. After it, open and
 * next return EINVAL.
 */
int ps_wal_iter_open(ps_wal *w, ps_lsn from_lsn, ps_wal_iter **out);

/*
 * *buf stays valid until the next call or close. ENOENT at end of log.
 * EBADMSG if a segment changed since open.
 */
int ps_wal_iter_next(ps_wal_iter *it, ps_lsn *lsn, const void **buf, usize *len);

/* Passing NULL is safe. */
void ps_wal_iter_close(ps_wal_iter *it);

/* Close after ps_close, since the store's log hooks use the WAL. Does not flush. */
void ps_wal_close(ps_wal *w);

/* ---- recovery ------------------------------------------------------------ */

/* Defined by the recovery caller. */
typedef struct ps_apply_ctx ps_apply_ctx;

/*
 * Commit exactly one operation at lsn before returning 0. Abort on error
 * and return a nonzero error code. rec is valid only during the call.
 */
typedef int (*ps_apply_fn)(ps_apply_ctx *ctx, ps_store *s, ps_lsn lsn, const void *rec, usize len);

/*
 * Passes each record above the durable LSN to apply, in order. Call once
 * right after ps_wal_open, before any operation or append. apply must commit
 * one operation at the record's LSN. Stops at the first error. To retry,
 * close and reopen the store and WAL.
 */
int ps_recover(ps_store *store, ps_wal *wal, ps_apply_fn apply, ps_apply_ctx *ctx);

PS_EXTERN_C_END
