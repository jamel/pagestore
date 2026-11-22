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

#include "pagestore/compiler.h"
#include "pagestore/pagestore.h"
#include "pagestore/types.h"

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
 * Opens the WAL in waldir (NULL = <dir>/wal of a ps_open store; EINVAL for
 * a ps_open_backend store) through the store's VFS. Creates a missing waldir
 * (its parent must exist) and fsyncs its parent, then locks <waldir>/LOCK
 * (EBUSY if held). cfg NULL uses the defaults; EINVAL outside the limits.
 * Startup only: no operation in progress, one thread.
 *
 * Validation starts from the store's durable LSN D. Leftover *.wal.tmp
 * files are deleted. With no segments, a fresh store (D = 0) gets
 * segment 1 and a store with D != 0 has lost its log (EBADMSG). Otherwise
 * the anchor is the newest segment whose prev_lsn <= D (none: EBADMSG).
 * From the anchor to the newest segment the sequence numbers must be
 * contiguous and each prev_lsn must equal the last LSN of the segment
 * before; every segment but the newest must hold valid records exactly up
 * to its end of file; the newest ends at its first invalid record (a torn
 * tail). Any violation is EBADMSG. Segments older than the anchor are
 * deleted unread. The newest segment is then sealed (truncated to its last
 * valid record and fsynced; a failure fails the open) and a fresh segment
 * is published for new appends, so a torn record's bytes can never
 * reappear after a later crash. I/O errors are returned as they are.
 *
 * The last valid LSN L of the log becomes the last appended LSN: appends
 * need lsn > L. After ps_recover that is any lsn > ps_committed_lsn(store).
 * On success the store's log hooks are set to ps_wal_flush /
 * ps_wal_truncate.
 */
int ps_wal_open(ps_store *store, const char *waldir, const ps_wal_config *cfg, ps_wal **out);

/*
 * Writer only. EINVAL if lsn does not exceed the last LSN in the log or len
 * exceeds max_record_size. ENOMEM, and ENOSPC or EDQUOT when starting a new
 * segment, leave the record unwritten and can be retried. After a fatal
 * error, append and flush return EIO until reopen.
 */
int ps_wal_append(ps_wal *w, u64 lsn, const void *buf, usize len);

/*
 * Makes records through lsn durable, up to the last appended LSN. Any
 * thread. A write or sync failure is fatal: flushes and appends return EIO,
 * and store checkpoints that need the WAL fail.
 */
int ps_wal_flush(ps_wal *w, u64 lsn);

/*
 * Drops segments that end at or below lsn, but keeps the newest. Any thread.
 * While iterators are open, deletion waits for a later call. An unlink error
 * is returned without failing the WAL.
 */
int ps_wal_truncate(ps_wal *w, u64 lsn);

/*
 * Starts at the first retained record with LSN >= from_lsn. One thread per
 * iterator. Iterators work only before the first append. After it, open and
 * next return EINVAL.
 */
int ps_wal_iter_open(ps_wal *w, u64 from_lsn, ps_wal_iter **out);

/*
 * *buf stays valid until the next call or close. ENOENT at end of log.
 * EBADMSG if a segment changed since open.
 */
int ps_wal_iter_next(ps_wal_iter *it, u64 *lsn, const void **buf, usize *len);

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
typedef int (*ps_apply_fn)(ps_apply_ctx *ctx, ps_store *store, u64 lsn, const void *rec, usize len);

/*
 * Replays the log: calls apply, in LSN order, for every WAL
 * record above ps_durable_lsn(store); records <= the durable LSN are already
 * in the store and are skipped. Call it right after ps_wal_open, before any
 * operation and any ps_wal_append (one thread, startup only).
 *
 * Contract and errors:
 * - EINVAL (logged) if an argument is NULL, wal was opened for another
 *   store, an operation is in progress on this thread, an operation was
 *   committed since open (committed LSN != durable LSN: e.g. a second
 *   ps_recover after one that replayed records), or the WAL was appended to.
 * - After apply returns 0, the committed LSN must equal the record's LSN
 *   (one operation per record, at that LSN). Otherwise returns EINVAL.
 *   Operation LSNs strictly increase, so each record is applied at most once
 *   to the same store state.
 * - An operation apply leaves open on this thread is aborted; ps_recover
 *   then returns apply's error, or EINVAL if apply returned 0.
 * - A non-zero return from apply stops the replay and is returned; so are
 *   iterator errors (EBADMSG, EIO, ENOMEM).
 * On any failure the records before the failing one stay committed (and
 * may be checkpointed); the rest are not applied. The caller should close
 * the store and the WAL: their LSNs stay reserved in the WAL (appends need
 * a higher LSN), and a later startup replays from the store's durable LSN
 * again, which is idempotent for the records already checkpointed.
 *
 * On success the committed LSN is the last replayed record's LSN (the
 * durable LSN if there was none), and new operations use any higher LSN.
 * The checkpoint thread may checkpoint replayed operations meanwhile; WAL
 * truncation waits until the replay ends.
 */
int ps_recover(ps_store *store, ps_wal *wal, ps_apply_fn apply, ps_apply_ctx *ctx);

PS_EXTERN_C_END
