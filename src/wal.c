/*
 * Each WAL segment header records the latest LSN before that segment.
 * Rotation seals and syncs the current segment before publishing its
 * successor, so a published header links back to durable records.
 */

#include "pagestore/wal.h"

#include "crc32c.h"
#include "format.h"
#include "store.h"
#include "util.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * WAL write path.
 *
 * Locks: io_mu (the "sync lock") serializes every file operation: buffer
 * drains, direct record writes, fsync, rotation and truncation, and guards
 * the I/O state below. buf_mu guards the append buffer and the last
 * appended LSN. Order: io_mu, then buf_mu; nothing else is taken while
 * either is held (the WAL never calls into the store under them; log lines
 * are emitted after unlocking). The store calls ps_wal_flush and
 * ps_wal_truncate from its hooks with ckpt_mu held, so ckpt_mu comes before
 * both, and the WAL never calls ps_checkpoint or ps_set_log_hooks from a hook.
 *
 * Durability boundary: `written` is the last LSN whose bytes were handed to
 * the OS, `synced` the last LSN known durable. A flush that finds
 * synced >= its target under io_mu returns at once (it shares the fsync of
 * the flush that held io_mu before it).
 */

#define WAL_MIN_SEGMENT_SIZE ((u64) 64 * 1024)
#define WAL_RECORD_OVERHEAD ((u64) PS_WAL_SEG_HEADER_SIZE + PS_WAL_REC_HEADER_SIZE)
#define WAL_NAME_LEN 32

struct ps_log_hooks_ctx {
    struct ps_wal *w;
};

struct ps_wal {
    ps_store *store; /* for logging while the store is open */
    ps_vfs vfs;
    ps_wal_config cfg;
    char *dir;
    char *path; /* scratch path, under io_mu after open */
    char *path2;
    usize dir_len;
    ps_log_hooks_ctx hctx;

    pthread_mutex_t io_mu;
    pthread_mutex_t buf_mu;
    bool io_init;
    bool buf_init;

    ps_file lock;
    bool have_lock;

    /* io_mu */
    ps_file seg;
    bool have_seg;
    u64 cur_seq;
    u64 file_off; /* excludes records still buffered */
    u64 written;
    u64 synced;
    bool sealed; /* set until the next segment is published */
    u64 first_seq; /* oldest live segment */
    u64 *prev_lsns; /* prev_lsn of segments first_seq..cur_seq */
    usize nsegs;
    usize segs_cap;

    /* buf_mu */
    u8 *buf;
    usize buf_len;
    u64 appended;

    /* appender only */
    u64 seg_end; /* includes bytes still buffered */

    _Atomic bool failed;
};

void ps_wal_config_init(ps_wal_config *cfg)
{
    cfg->segment_size = (u64) 64 << 20;
    cfg->buffer_size = (usize) 1 << 20;
    cfg->max_record_size = (u32) 16 << 20;
}

static int check_config(const ps_wal_config *cfg)
{
    if (cfg->segment_size < WAL_MIN_SEGMENT_SIZE) {
        return EINVAL;
    }
    if ((u64) cfg->max_record_size > cfg->segment_size - WAL_RECORD_OVERHEAD) {
        return EINVAL;
    }
    /* Record padding must fit even when the segment size is unaligned. */
    if ((u64) ps_wal_rec_size(cfg->max_record_size) > cfg->segment_size - PS_WAL_SEG_HEADER_SIZE) {
        return EINVAL;
    }
    return 0;
}

/* ---- paths --------------------------------------------------------------- */

static char *str_dup(const char *s)
{
    const usize n = strlen(s) + 1;
    char *d = malloc(n);
    if (d != NULL) {
        memcpy(d, s, n);
    }
    return d;
}

static char *str_join(const char *a, const char *b)
{
    const usize na = strlen(a);
    const usize nb = strlen(b);
    char *s = malloc(na + nb + 1);
    if (s != NULL) {
        memcpy(s, a, na);
        memcpy(s + na, b, nb + 1);
    }
    return s;
}

/* For example, "/a/b" gives "/a", "/a" gives "/", and "a" gives ".". */
static char *parent_of(const char *path)
{
    usize n = strlen(path);
    while (n > 1 && path[n - 1] == '/') {
        n--;
    }
    while (n > 0 && path[n - 1] != '/') {
        n--;
    }
    if (n == 0) {
        return str_dup(".");
    }
    while (n > 1 && path[n - 1] == '/') {
        n--;
    }
    char *s = malloc(n + 1);
    if (s != NULL) {
        memcpy(s, path, n);
        s[n] = '\0';
    }
    return s;
}

/* The directory prefix is already in dst. */
static const char *wal_path(ps_wal *w, char *dst, const char *name)
{
    snprintf(dst + w->dir_len, WAL_NAME_LEN + 2, "/%s", name);
    return dst;
}

static const char *seg_path(ps_wal *w, char *dst, u64 seq, bool tmp)
{
    char name[WAL_NAME_LEN];
    snprintf(name, sizeof(name), "%016llx.wal%s", (unsigned long long) seq, tmp ? ".tmp" : "");
    return wal_path(w, dst, name);
}

/* Accept only 16 lowercase hex digits followed by .wal or .wal.tmp. */
static bool parse_seg_name(const char *name, u64 *seq, bool *tmp)
{
    u64 v = 0;
    for (u32 i = 0; i < 16; i++) {
        const char c = name[i];
        u64 d = 0;
        if (c >= '0' && c <= '9') {
            d = (u64) (c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (u64) (c - 'a' + 10);
        } else {
            return false;
        }
        v = v << 4 | d;
    }
    if (strcmp(name + 16, ".wal") == 0) {
        *tmp = false;
    } else if (strcmp(name + 16, ".wal.tmp") == 0) {
        *tmp = true;
    } else {
        return false;
    }
    *seq = v;
    return true;
}

/* ---- failure ------------------------------------------------------------- */

/* Hold io_mu here and report the failure after unlocking. */
static void set_failed(ps_wal *w)
{
    atomic_store_explicit(&w->failed, true, memory_order_release);
}

static bool is_failed(ps_wal *w)
{
    return atomic_load_explicit(&w->failed, memory_order_acquire);
}

static void log_failure(ps_wal *w, const char *what, int err)
{
    char buf[128];
    ps_log(
        w->store,
        PS_LOG_ERROR,
        "WAL %s failed: %s (error %d); the WAL is failed until reopen",
        what,
        ps_strerror(err, buf, sizeof(buf)),
        err);
}

/* ---- I/O under io_mu ----------------------------------------------------- */

/* Write buffered records to the newest segment. Advance written on success. */
static int drain(ps_wal *w)
{
    pthread_mutex_lock(&w->buf_mu);
    int rv = 0;
    if (w->buf_len > 0) {
        rv = w->vfs.pwrite(w->vfs.ctx, &w->seg, w->buf, w->buf_len, w->file_off);
        if (rv == 0) {
            w->file_off += w->buf_len;
            w->written = w->appended;
            w->buf_len = 0;
        }
    }
    pthread_mutex_unlock(&w->buf_mu);
    return rv;
}

/* Larger records bypass the append buffer. The caller has drained it first. */
static int write_direct(ps_wal *w, u64 lsn, const void *payload, u32 len)
{
    u8 hdr[PS_WAL_REC_HEADER_SIZE];
    u8 lsn_le[8];
    ps_put_le64(lsn_le, lsn);
    ps_put_le32(hdr, len);
    ps_put_le32(hdr + 4, ps_crc32c(ps_crc32c(0, lsn_le, sizeof(lsn_le)), payload, len));
    ps_put_le64(hdr + 8, lsn);
    const usize size = ps_wal_rec_size(len);
    const usize pad = size - PS_WAL_REC_HEADER_SIZE - len;
    u64 off = w->file_off;
    int rv = w->vfs.pwrite(w->vfs.ctx, &w->seg, hdr, sizeof(hdr), off);
    off += sizeof(hdr);
    if (rv == 0 && len > 0) {
        rv = w->vfs.pwrite(w->vfs.ctx, &w->seg, payload, len, off);
        off += len;
    }
    if (rv == 0 && pad > 0) {
        const u8 zeros[8] = {0};
        rv = w->vfs.pwrite(w->vfs.ctx, &w->seg, zeros, pad, off);
    }
    if (rv != 0) {
        return rv;
    }
    w->file_off += size;
    w->written = lsn;
    return 0;
}

/*
 * Publishes segment seq with prev_lsn: create
 * <seq>.wal.tmp, preallocate it, write its header, fsync it, rename it to
 * <seq>.wal and fsync the directory. On success *out is the open segment.
 * *clean is set when nothing is left behind and the WAL may stay healthy:
 * ENOSPC from creating or preallocating the file (the tmp file is deleted).
 */
static int publish(ps_wal *w, u64 seq, u64 prev_lsn, ps_file *out, bool *clean)
{
    *clean = false;
    ps_file f;
    int rv =
        w->vfs.open(w->vfs.ctx, seg_path(w, w->path, seq, true), PS_VFS_CREATE | PS_VFS_EXCL, &f);
    if (rv != 0) {
        *clean = rv == ENOSPC;
        return rv;
    }
    rv = w->vfs.preallocate(w->vfs.ctx, &f, w->cfg.segment_size);
    if (rv != 0) {
        w->vfs.close(w->vfs.ctx, &f);
        if (rv == ENOSPC) {
            /* The retry uses exclusive create, so first remove this temp file. */
            *clean = w->vfs.unlink(w->vfs.ctx, seg_path(w, w->path, seq, true)) == 0;
        }
        return rv;
    }
    u8 hdr[PS_WAL_SEG_HEADER_SIZE];
    const ps_wal_seg_header h = {.seq = seq, .prev_lsn = prev_lsn};
    ps_wal_seg_header_encode(&h, hdr);
    rv = w->vfs.pwrite(w->vfs.ctx, &f, hdr, sizeof(hdr), 0);
    if (rv == 0) {
        rv = w->vfs.fsync(w->vfs.ctx, &f);
    }
    if (rv == 0) {
        rv = w->vfs.rename(
            w->vfs.ctx,
            seg_path(w, w->path, seq, true),
            seg_path(w, w->path2, seq, false));
    }
    if (rv == 0) {
        rv = w->vfs.fsync_dir(w->vfs.ctx, w->dir);
    }
    if (rv != 0) {
        w->vfs.close(w->vfs.ctx, &f);
        return rv;
    }
    *out = f;
    return 0;
}

/* Reserve room for a new prev_lsn so publishing needs no later allocation. */
static int reserve_seg(ps_wal *w)
{
    if (w->nsegs < w->segs_cap) {
        return 0;
    }
    const usize cap = w->segs_cap == 0 ? 8 : w->segs_cap * 2;
    u64 *p = realloc(w->prev_lsns, cap * sizeof(*p));
    if (p == NULL) {
        return ENOMEM;
    }
    w->prev_lsns = p;
    w->segs_cap = cap;
    return 0;
}

/* A retry after a clean publish failure keeps the seal and skips straight to publishing. */
static int rotate(ps_wal *w, bool *fatal)
{
    *fatal = false;
    int rv = 0;
    if (!w->sealed) {
        rv = drain(w);
        if (rv == 0) {
            rv = w->vfs.truncate(w->vfs.ctx, &w->seg, w->file_off);
        }
        if (rv == 0) {
            rv = w->vfs.fsync(w->vfs.ctx, &w->seg);
        }
        if (rv != 0) {
            *fatal = true;
            return rv;
        }
        w->synced = w->written;
        w->sealed = true;
    }
    rv = reserve_seg(w);
    if (rv != 0) {
        return rv;
    }
    ps_file f;
    bool clean = false;
    /* For an empty segment, written still equals its prev_lsn. */
    rv = publish(w, w->cur_seq + 1, w->written, &f, &clean);
    if (rv != 0) {
        *fatal = !clean;
        return rv;
    }
    w->vfs.close(w->vfs.ctx, &w->seg);
    w->seg = f;
    w->cur_seq++;
    w->prev_lsns[w->nsegs++] = w->written;
    w->file_off = PS_WAL_SEG_HEADER_SIZE;
    w->sealed = false;
    return 0;
}

/* ---- public API ---------------------------------------------------------- */

int ps_wal_append(ps_wal *w, u64 lsn, const void *buf, usize len)
{
    if (is_failed(w)) {
        return EIO;
    }
    if (lsn <= w->appended || len > w->cfg.max_record_size) {
        return EINVAL;
    }
    const u32 n = (u32) len;
    const usize size = ps_wal_rec_size(n);
    bool fatal = false;
    int rv = 0;

    if (w->sealed || w->seg_end + size > w->cfg.segment_size) {
        pthread_mutex_lock(&w->io_mu);
        rv = rotate(w, &fatal);
        if (fatal) {
            set_failed(w);
        }
        pthread_mutex_unlock(&w->io_mu);
        if (rv != 0) {
            if (fatal) {
                log_failure(w, "rotation", rv);
                return EIO;
            }
            return rv;
        }
        w->seg_end = PS_WAL_SEG_HEADER_SIZE;
    }

    if (size <= w->cfg.buffer_size) {
        /* A concurrent drain can only make more room. */
        pthread_mutex_lock(&w->buf_mu);
        const bool room = w->buf_len + size <= w->cfg.buffer_size;
        pthread_mutex_unlock(&w->buf_mu);
        if (!room) {
            pthread_mutex_lock(&w->io_mu);
            rv = drain(w);
            if (rv != 0) {
                set_failed(w);
            }
            pthread_mutex_unlock(&w->io_mu);
            if (rv != 0) {
                log_failure(w, "write", rv);
                return EIO;
            }
        }
        pthread_mutex_lock(&w->buf_mu);
        ps_wal_rec_encode(w->buf + w->buf_len, lsn, buf, n);
        w->buf_len += size;
        w->appended = lsn;
        pthread_mutex_unlock(&w->buf_mu);
    } else {
        pthread_mutex_lock(&w->io_mu);
        rv = drain(w);
        if (rv == 0) {
            rv = write_direct(w, lsn, buf, n);
        }
        if (rv != 0) {
            set_failed(w);
        } else {
            pthread_mutex_lock(&w->buf_mu);
            w->appended = lsn;
            pthread_mutex_unlock(&w->buf_mu);
        }
        pthread_mutex_unlock(&w->io_mu);
        if (rv != 0) {
            log_failure(w, "write", rv);
            return EIO;
        }
    }
    w->seg_end += size;
    return 0;
}

int ps_wal_flush(ps_wal *w, u64 lsn)
{
    if (is_failed(w)) {
        return EIO;
    }
    pthread_mutex_lock(&w->io_mu);
    if (is_failed(w)) {
        pthread_mutex_unlock(&w->io_mu);
        return EIO;
    }
    pthread_mutex_lock(&w->buf_mu);
    const u64 target = PS_MIN(lsn, w->appended);
    pthread_mutex_unlock(&w->buf_mu);
    if (w->synced >= target) {
        pthread_mutex_unlock(&w->io_mu);
        return 0;
    }
    const char *what = "write";
    int rv = drain(w);
    if (rv == 0) {
        what = "fsync";
        rv = w->vfs.fsync(w->vfs.ctx, &w->seg);
    }
    if (rv == 0) {
        w->synced = w->written;
    } else {
        set_failed(w);
    }
    pthread_mutex_unlock(&w->io_mu);
    if (rv != 0) {
        log_failure(w, what, rv);
        return EIO;
    }
    return 0;
}

int ps_wal_truncate(ps_wal *w, u64 lsn)
{
    pthread_mutex_lock(&w->io_mu);
    int rv = 0;
    usize drop = 0;
    while (w->nsegs - drop >= 2 && w->prev_lsns[drop + 1] <= lsn) {
        rv = w->vfs.unlink(w->vfs.ctx, seg_path(w, w->path, w->first_seq + drop, false));
        if (rv != 0 && rv != ENOENT) {
            break;
        }
        rv = 0;
        drop++;
    }
    if (drop > 0) {
        memmove(w->prev_lsns, w->prev_lsns + drop, (w->nsegs - drop) * sizeof(*w->prev_lsns));
        w->nsegs -= drop;
        w->first_seq += drop;
    }
    const u64 seq = w->first_seq;
    pthread_mutex_unlock(&w->io_mu);
    if (rv != 0) {
        char buf[128];
        ps_log(
            w->store,
            PS_LOG_WARN,
            "WAL truncate: deleting segment %016llx failed: %s (error %d)",
            (unsigned long long) seq,
            ps_strerror(rv, buf, sizeof(buf)),
            rv);
    }
    return rv;
}

/* ---- hooks --------------------------------------------------------------- */

static int hook_flush_log(ps_log_hooks_ctx *ctx, u64 lsn)
{
    return ps_wal_flush(ctx->w, lsn);
}

static void hook_checkpoint_done(ps_log_hooks_ctx *ctx, u64 lsn)
{
    ps_wal_truncate(ctx->w, lsn); /* Logs unlink errors and retries leftovers on a later call. */
}

/* ---- open / close -------------------------------------------------------- */

/* Directory listing at open: segment count and leftover tmp files. */
struct ps_vfs_list_ctx {
    u64 nsegs;
    u64 *tmps;
    usize ntmps;
    usize tmps_cap;
    int err;
};

static int list_entry(ps_vfs_list_ctx *lc, const char *name)
{
    u64 seq = 0;
    bool tmp = false;
    if (!parse_seg_name(name, &seq, &tmp)) {
        return 0;
    }
    if (!tmp) {
        lc->nsegs++;
        return 0;
    }
    if (lc->ntmps == lc->tmps_cap) {
        const usize cap = lc->tmps_cap == 0 ? 4 : lc->tmps_cap * 2;
        u64 *p = realloc(lc->tmps, cap * sizeof(*p));
        if (p == NULL) {
            lc->err = ENOMEM;
            return ENOMEM;
        }
        lc->tmps = p;
        lc->tmps_cap = cap;
    }
    lc->tmps[lc->ntmps++] = seq;
    return 0;
}

void ps_wal_close(ps_wal *w)
{
    if (w == NULL) {
        return;
    }
    if (w->have_seg) {
        w->vfs.close(w->vfs.ctx, &w->seg);
    }
    if (w->have_lock) {
        w->vfs.close(w->vfs.ctx, &w->lock);
    }
    if (w->buf_init) {
        pthread_mutex_destroy(&w->buf_mu);
    }
    if (w->io_init) {
        pthread_mutex_destroy(&w->io_mu);
    }
    free(w->prev_lsns);
    free(w->buf);
    free(w->path);
    free(w->path2);
    free(w->dir);
    free(w);
}

/* Creates the directory if missing (fsyncing its parent) and takes the lock. */
static int open_dir(ps_wal *w)
{
    int rv = w->vfs.mkdir(w->vfs.ctx, w->dir);
    if (rv == 0) {
        char *parent = parent_of(w->dir);
        if (parent == NULL) {
            return ENOMEM;
        }
        rv = w->vfs.fsync_dir(w->vfs.ctx, parent);
        free(parent);
        if (rv != 0) {
            return rv;
        }
    } else if (rv != EEXIST) {
        return rv;
    }
    rv = w->vfs.lock(w->vfs.ctx, wal_path(w, w->path, "LOCK"), &w->lock);
    if (rv != 0) {
        return rv;
    }
    w->have_lock = true;
    return 0;
}

static int open_segments(ps_wal *w)
{
    ps_vfs_list_ctx lc;
    memset(&lc, 0, sizeof(lc));
    int rv = w->vfs.list(w->vfs.ctx, w->dir, list_entry, &lc);
    if (rv == 0) {
        /* Leftover tmp files are never complete segments. */
        for (usize i = 0; i < lc.ntmps && rv == 0; i++) {
            rv = w->vfs.unlink(w->vfs.ctx, seg_path(w, w->path, lc.tmps[i], true));
            if (rv == ENOENT) {
                rv = 0;
            }
        }
    }
    free(lc.tmps);
    if (rv != 0) {
        return rv;
    }
    if (lc.nsegs > 0) {
        /* Opening a directory with existing segments is unsupported. */
        ps_log(
            w->store,
            PS_LOG_ERROR,
            "WAL %s: reopening existing segments is not supported",
            w->dir);
        return EBADMSG;
    }
    if (ps_durable_lsn(w->store) != 0) {
        ps_log(
            w->store,
            PS_LOG_ERROR,
            "WAL %s has no segments but the store's durable LSN is %llu: the log was lost",
            w->dir,
            (unsigned long long) ps_durable_lsn(w->store));
        return EBADMSG;
    }
    rv = reserve_seg(w);
    if (rv != 0) {
        return rv;
    }
    bool clean = false;
    rv = publish(w, 1, 0, &w->seg, &clean);
    if (rv != 0) {
        return rv;
    }
    w->have_seg = true;
    w->cur_seq = 1;
    w->first_seq = 1;
    w->prev_lsns[0] = 0;
    w->nsegs = 1;
    w->file_off = PS_WAL_SEG_HEADER_SIZE;
    w->seg_end = PS_WAL_SEG_HEADER_SIZE;
    return 0;
}

int ps_wal_open(ps_store *store, const char *waldir, const ps_wal_config *cfg, ps_wal **out)
{
    if (store == NULL || out == NULL || (waldir != NULL && waldir[0] == '\0')) {
        return EINVAL;
    }
    ps_wal_config c;
    if (cfg != NULL) {
        c = *cfg;
    } else {
        ps_wal_config_init(&c);
    }
    if (check_config(&c) != 0) {
        ps_log(store, PS_LOG_ERROR, "WAL config out of range");
        return EINVAL;
    }
    if (waldir == NULL && ps_store_dir(store) == NULL) {
        ps_log(store, PS_LOG_ERROR, "WAL: a NULL waldir needs a store opened with ps_open");
        return EINVAL;
    }

    ps_wal *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return ENOMEM;
    }
    w->store = store;
    w->vfs = *ps_store_vfs(store);
    w->cfg = c;
    w->hctx.w = w;
    atomic_init(&w->failed, false);
    w->dir = waldir != NULL ? str_dup(waldir) : str_join(ps_store_dir(store), "/wal");
    if (w->dir != NULL) {
        w->dir_len = strlen(w->dir);
        w->path = malloc(w->dir_len + WAL_NAME_LEN + 2);
        w->path2 = malloc(w->dir_len + WAL_NAME_LEN + 2);
    }
    if (c.buffer_size > 0) {
        w->buf = malloc(c.buffer_size);
    }
    if (w->dir == NULL || w->path == NULL || w->path2 == NULL ||
        (c.buffer_size > 0 && w->buf == NULL)) {
        ps_wal_close(w);
        return ENOMEM;
    }
    memcpy(w->path, w->dir, w->dir_len + 1);
    memcpy(w->path2, w->dir, w->dir_len + 1);
    if (pthread_mutex_init(&w->io_mu, NULL) != 0) {
        ps_wal_close(w);
        return ENOMEM;
    }
    w->io_init = true;
    if (pthread_mutex_init(&w->buf_mu, NULL) != 0) {
        ps_wal_close(w);
        return ENOMEM;
    }
    w->buf_init = true;

    int rv = open_dir(w);
    if (rv == 0) {
        rv = open_segments(w);
    }
    if (rv != 0) {
        ps_wal_close(w);
        return rv;
    }
    const ps_log_hooks hooks = {
        .flush_log = hook_flush_log,
        .checkpoint_done = hook_checkpoint_done,
        .ctx = &w->hctx,
    };
    ps_set_log_hooks(store, &hooks);
    *out = w;
    return 0;
}
