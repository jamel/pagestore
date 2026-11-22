/*
 * Replays WAL records above the store's durable LSN through the caller's
 * apply callback. A restart skips any records checkpointed during replay.
 */

#include "pagestore/wal.h"
#include "store.h"
#include "wal_internal.h"

#include <errno.h>

static int recover_misuse(ps_store *s, const char *why)
{
    ps_log(s, PS_LOG_ERROR, "ps_recover: %s", why);
    return EINVAL;
}

int ps_recover(ps_store *store, ps_wal *wal, ps_apply_fn apply, ps_apply_ctx *ctx)
{
    if (store == NULL) {
        return EINVAL;
    }
    if (wal == NULL) {
        return recover_misuse(store, "wal is NULL");
    }
    if (apply == NULL) {
        return recover_misuse(store, "apply is NULL");
    }
    if (ps_wal_store(wal) != store) {
        return recover_misuse(store, "the WAL was opened for another store");
    }
    if (ps_op_in_progress(store)) {
        return recover_misuse(store, "an operation is in progress");
    }
    const u64 d = ps_durable_lsn(store);
    if (ps_committed_lsn(store) != d) {
        return recover_misuse(store, "operations were committed since the store was opened");
    }
    /* No LSN can follow UINT64_MAX. Avoid wrapping the replay start. */
    if (d == UINT64_MAX) {
        return 0;
    }
    ps_wal_iter *it = NULL;
    int rv = ps_wal_iter_open(wal, d + 1, &it);
    if (rv != 0) {
        return rv;
    }
    u64 n = 0;
    u64 first = 0;
    u64 lsn = 0;
    for (;;) {
        const void *rec = NULL;
        usize len = 0;
        rv = ps_wal_iter_next(it, &lsn, &rec, &len);
        if (rv == ENOENT) {
            rv = 0;
            break;
        }
        if (rv != 0) {
            char buf[128];
            ps_log(
                store,
                PS_LOG_ERROR,
                "ps_recover: reading the WAL after LSN %llu failed: %s (error %d)",
                (unsigned long long) (n > 0 ? lsn : d),
                ps_strerror(rv, buf, sizeof(buf)),
                rv);
            break;
        }
        rv = apply(ctx, store, lsn, rec, len);
        if (ps_op_active_here(store)) {
            ps_op_abort(store);
            ps_log(
                store,
                PS_LOG_ERROR,
                "ps_recover: apply left the operation of record %llu open; aborted it",
                (unsigned long long) lsn);
            if (rv == 0) {
                rv = EINVAL;
            }
            break;
        }
        if (rv != 0) {
            char buf[128];
            ps_log(
                store,
                PS_LOG_ERROR,
                "ps_recover: applying record %llu failed: %s (error %d)",
                (unsigned long long) lsn,
                ps_strerror(rv, buf, sizeof(buf)),
                rv);
            break;
        }
        const u64 c = ps_committed_lsn(store);
        if (c != lsn) {
            ps_log(
                store,
                PS_LOG_ERROR,
                "ps_recover: after applying record %llu the committed LSN is %llu "
                "(apply must commit one operation at the record's LSN)",
                (unsigned long long) lsn,
                (unsigned long long) c);
            rv = EINVAL;
            break;
        }
        if (n == 0) {
            first = lsn;
        }
        n++;
    }
    ps_wal_iter_close(it);
    if (rv == 0) {
        if (n > 0) {
            ps_log(
                store,
                PS_LOG_INFO,
                "recovered %llu WAL records (LSNs %llu..%llu) above the durable LSN %llu",
                (unsigned long long) n,
                (unsigned long long) first,
                (unsigned long long) lsn,
                (unsigned long long) d);
        } else {
            ps_log(
                store,
                PS_LOG_INFO,
                "no WAL records above the durable LSN %llu",
                (unsigned long long) d);
        }
    }
    return rv;
}
