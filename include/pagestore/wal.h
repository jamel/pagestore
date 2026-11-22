#pragma once

/*
 * Write-ahead log: segment files <waldir>/%016llx.wal, rotated
 * at segment_size, holding records (lsn, payload) with strictly increasing
 * LSNs. The WAL is opened from an open store (it needs the store's durable
 * LSN and installs the store's log hooks) and closed after it:
 *
 *     ps_open(dir, &cfg, flags, &store);
 *     ps_wal_open(store, NULL, &wcfg, &wal);
 *     ... ps_wal_append / ps_wal_flush / ps_op_commit ...
 *     ps_close(store);
 *     ps_wal_close(wal);
 *
 * Any write or fsync error puts the WAL in a failed state: that call and
 * every later ps_wal_append / ps_wal_flush return EIO until reopen, and the
 * store's flush_log hook fails, which fails the store.
 */

#include "pagestore/compiler.h"
#include "pagestore/pagestore.h"
#include "pagestore/types.h"

PS_EXTERN_C_BEGIN

typedef struct ps_wal ps_wal;

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
 * With no segments, a fresh store (durable LSN 0) gets segment 1; a store
 * with a non-zero durable LSN has lost its log (EBADMSG). On success the
 * store's log hooks are set to ps_wal_flush / ps_wal_truncate.
 * Startup only: no operation in progress, one thread.
 *
 * Opening a directory that already holds segments returns EBADMSG.
 */
int ps_wal_open(ps_store *store, const char *waldir, const ps_wal_config *cfg, ps_wal **out);

/*
 * Appends a record; one thread (the writer). EINVAL if lsn is not above the
 * last appended LSN or len > max_record_size; ENOMEM, or ENOSPC when the
 * disk is full at rotation: nothing was written, the WAL stays healthy and
 * the call may be retried. EIO if the WAL has failed or fails now.
 * The record is durable only after ps_wal_flush.
 */
int ps_wal_append(ps_wal *w, u64 lsn, const void *buf, usize len);

/*
 * Makes records through lsn durable, up to the last appended LSN. Any
 * thread. A write or sync failure is fatal: flushes and appends return EIO,
 * and store checkpoints that need the WAL fail.
 */
int ps_wal_flush(ps_wal *w, u64 lsn);

/*
 * Deletes the oldest segments whose records are all <= lsn (the next
 * segment's prev_lsn <= lsn), never the newest one. Any thread. Returns
 * the unlink error, if any (the WAL stays healthy).
 */
int ps_wal_truncate(ps_wal *w, u64 lsn);

/* Close after ps_close. The WAL close performs no flush. */
void ps_wal_close(ps_wal *w);

PS_EXTERN_C_END
