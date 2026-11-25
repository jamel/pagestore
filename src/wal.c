/*
 * Each WAL segment header records the latest LSN before that segment.
 * Rotation seals and syncs the current segment before publishing its
 * successor, so a published header links back to durable records.
 */

#include "crc32c.h"
#include "format.h"
#include "store.h"
#include "util.h"
#include "wal_internal.h"

#include "pagestore/bytes.h"
#include "pagestore/pagestore.h"
#include "pagestore/vfs.h"
#include "pagestore/wal.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Take the store's ckpt_mu (when called from a checkpoint), then io_mu,
 * then buf_mu. Log only after releasing both WAL locks.
 *
 * appended includes buffered records. written has reached the OS and
 * synced has reached durable storage. Check synced under io_mu so callers
 * can share an earlier flush's fsync.
 */

#define WAL_MIN_SEGMENT_SIZE ((u64) 64 * 1024)
#define WAL_RECORD_OVERHEAD ((u64) PS_WAL_SEG_HEADER_SIZE + PS_WAL_REC_HEADER_SIZE)
#define WAL_NAME_LEN 32

struct ps_log_hooks_ctx {
    struct ps_wal *w;
};

struct ps_wal {
    ps_store *store;
    ps_logger logger; /* the store's, copied at open */
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
    ps_lsn written;
    ps_lsn synced;
    bool sealed; /* set until the next segment is published */
    u64 first_seq; /* oldest live segment */
    ps_lsn *prev_lsns; /* prev_lsn of segments first_seq..cur_seq */
    usize nsegs;
    usize segs_cap;
    u32 iters; /* truncation skips deletion while iterators are open */

    /* buf_mu */
    u8 *buf;
    usize buf_len;
    ps_lsn appended;

    /* appender only */
    u64 seg_end; /* includes bytes still buffered */

    _Atomic bool failed;
    _Atomic bool appending; /* set by the first append call, even on error */
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
    snprintf(name, sizeof(name), "%016llx.wal%s", seq, tmp ? ".tmp" : "");
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
    PS_LOG_ERROR(
        &w->logger,
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
static int write_direct(ps_wal *w, ps_lsn lsn, const void *payload, u32 len)
{
    u8 hdr[PS_WAL_REC_HEADER_SIZE];
    u8 lsn_le[8];
    ps_put_le64(lsn_le, lsn.raw);
    ps_put_le32(hdr, len);
    ps_put_le32(hdr + 4, ps_crc32c(ps_crc32c(0, lsn_le, sizeof(lsn_le)), payload, len));
    ps_put_le64(hdr + 8, lsn.raw);
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

static bool is_disk_full(int err)
{
    return err == ENOSPC || err == EDQUOT;
}

/*
 * Publish only after the header and preallocation are durable. *clean
 * allows a retry after disk exhaustion during create or preallocation,
 * provided no temp file remains.
 */
static int publish(ps_wal *w, u64 seq, ps_lsn prev_lsn, ps_file *out, bool *clean)
{
    *clean = false;
    ps_file f;
    int rv =
        w->vfs.open(w->vfs.ctx, seg_path(w, w->path, seq, true), PS_VFS_CREATE | PS_VFS_EXCL, &f);
    if (rv != 0) {
        *clean = is_disk_full(rv);
        return rv;
    }
    rv = w->vfs.preallocate(w->vfs.ctx, &f, w->cfg.segment_size);
    if (rv != 0) {
        w->vfs.close(w->vfs.ctx, &f);
        if (is_disk_full(rv)) {
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
    ps_lsn *p = realloc(w->prev_lsns, cap * sizeof(*p));
    if (p == NULL) {
        return ENOMEM;
    }
    w->prev_lsns = p;
    w->segs_cap = cap;
    return 0;
}

/* A retry after a clean publish failure keeps the seal and skips straight to publishing. */
static int rotate(ps_wal *w, bool *fatal, const char **what)
{
    *fatal = false;
    *what = "rotation";
    int rv = 0;
    if (!w->sealed) {
        *what = "write";
        rv = drain(w);
        if (rv == 0) {
            *what = "seal";
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
    *what = "segment publish";
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

int ps_wal_append(ps_wal *w, ps_lsn lsn, const void *buf, usize len)
{
    atomic_store_explicit(&w->appending, true, memory_order_release);
    if (is_failed(w)) {
        return EIO;
    }
    if (ps_lsn_le(lsn, w->appended) || len > w->cfg.max_record_size) {
        return EINVAL;
    }
    const u32 n = (u32) len;
    const usize size = ps_wal_rec_size(n);
    bool fatal = false;
    int rv = 0;

    /* A concurrent flush can fail the WAL before we take io_mu. Recheck under it. */
    if (w->sealed || w->seg_end + size > w->cfg.segment_size) {
        pthread_mutex_lock(&w->io_mu);
        if (is_failed(w)) {
            pthread_mutex_unlock(&w->io_mu);
            return EIO;
        }
        const char *what = NULL;
        rv = rotate(w, &fatal, &what);
        if (fatal) {
            set_failed(w);
        }
        pthread_mutex_unlock(&w->io_mu);
        if (rv != 0) {
            if (fatal) {
                log_failure(w, what, rv);
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
            if (is_failed(w)) {
                pthread_mutex_unlock(&w->io_mu);
                return EIO;
            }
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
        if (is_failed(w)) {
            pthread_mutex_unlock(&w->io_mu);
            return EIO;
        }
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

int ps_wal_flush(ps_wal *w, ps_lsn lsn)
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
    const ps_lsn target = ps_lsn_lt(lsn, w->appended) ? lsn : w->appended;
    pthread_mutex_unlock(&w->buf_mu);
    if (ps_lsn_ge(w->synced, target)) {
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

int ps_wal_truncate(ps_wal *w, ps_lsn lsn)
{
    pthread_mutex_lock(&w->io_mu);
    int rv = 0;
    usize drop = 0;
    /* Recovery can be reading these files while the checkpoint runs. */
    while (w->iters == 0 && w->nsegs - drop >= 2 && ps_lsn_le(w->prev_lsns[drop + 1], lsn)) {
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
        PS_LOG_WARN(
            &w->logger,
            "WAL truncate: deleting segment %016llx failed: %s (error %d)",
            seq,
            ps_strerror(rv, buf, sizeof(buf)),
            rv);
    }
    return rv;
}

/* ---- hooks --------------------------------------------------------------- */

static int hook_flush_log(ps_log_hooks_ctx *ctx, ps_lsn lsn)
{
    return ps_wal_flush(ctx->w, lsn);
}

static void hook_checkpoint_done(ps_log_hooks_ctx *ctx, ps_lsn lsn)
{
    ps_wal_truncate(ctx->w, lsn); /* Logs unlink errors and retries leftovers on a later call. */
}

/* ---- open / close -------------------------------------------------------- */

/* Collect published segments and temp files separately. */
struct ps_vfs_list_ctx {
    u64 *segs;
    usize nsegs;
    usize segs_cap;
    u64 *tmps;
    usize ntmps;
    usize tmps_cap;
    int err;
};

static int push_seq(u64 **arr, usize *n, usize *cap, u64 seq)
{
    if (*n == *cap) {
        const usize c = *cap == 0 ? 8 : *cap * 2;
        u64 *p = realloc(*arr, c * sizeof(*p));
        if (p == NULL) {
            return ENOMEM;
        }
        *arr = p;
        *cap = c;
    }
    (*arr)[(*n)++] = seq;
    return 0;
}

static int list_entry(ps_vfs_list_ctx *lc, const char *name)
{
    u64 seq = 0;
    bool tmp = false;
    if (!parse_seg_name(name, &seq, &tmp)) {
        return 0;
    }
    const int rv = tmp ? push_seq(&lc->tmps, &lc->ntmps, &lc->tmps_cap, seq) :
                         push_seq(&lc->segs, &lc->nsegs, &lc->segs_cap, seq);
    if (rv != 0) {
        lc->err = rv;
    }
    return rv;
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

/* Sync the parent even on EEXIST. An earlier open may have missed that sync. */
static int open_dir(ps_wal *w)
{
    int rv = w->vfs.mkdir(w->vfs.ctx, w->dir);
    if (rv != 0 && rv != EEXIST) {
        return rv;
    }
    char *parent = parent_of(w->dir);
    if (parent == NULL) {
        return ENOMEM;
    }
    rv = w->vfs.fsync_dir(w->vfs.ctx, parent);
    free(parent);
    if (rv != 0) {
        return rv;
    }
    rv = w->vfs.lock(w->vfs.ctx, wal_path(w, w->path, "LOCK"), &w->lock);
    if (rv != 0) {
        return rv;
    }
    w->have_lock = true;
    return 0;
}

/* ---- reading segments ---------------------------------------------------- */

#define WAL_READ_CHUNK ((usize) 256 * 1024)

/* Read one segment through a window. The caller decides if an invalid tail is allowed. */
typedef struct seg_scan {
    ps_file f;
    bool have_f;
    u64 size;
    u64 off;
    ps_lsn last; /* starts at the header's prev_lsn */
    u8 *buf; /* window for file bytes [win_off, win_off + win_len) */
    usize cap;
    u64 win_off;
    usize win_len;
} seg_scan;

static void scan_close(const ps_vfs *vfs, seg_scan *sc)
{
    if (sc->have_f) {
        vfs->close(vfs->ctx, &sc->f);
        sc->have_f = false;
    }
}

static void scan_free(const ps_vfs *vfs, seg_scan *sc)
{
    scan_close(vfs, sc);
    free(sc->buf);
    sc->buf = NULL;
    sc->cap = 0;
    sc->win_len = 0;
}

/* Return n bytes at sc->off through *p, or leave it NULL if fewer bytes are available. */
static int scan_fill(const ps_vfs *vfs, seg_scan *sc, u64 n, const u8 **p)
{
    *p = NULL;
    if (n > sc->size - sc->off) {
        return 0;
    }
    if (sc->off >= sc->win_off && sc->off - sc->win_off + n <= sc->win_len) {
        *p = sc->buf + (sc->off - sc->win_off);
        return 0;
    }
    const u64 want = PS_MIN(PS_MAX(n, (u64) WAL_READ_CHUNK), sc->size - sc->off);
    if (want > (u64) SIZE_MAX) {
        return ENOMEM;
    }
    sc->win_len = 0;
    if (want > sc->cap) {
        free(sc->buf);
        sc->cap = 0;
        sc->buf = malloc((usize) want);
        if (sc->buf == NULL) {
            return ENOMEM;
        }
        sc->cap = (usize) want;
    }
    usize got = 0;
    const int rv = vfs->pread(vfs->ctx, &sc->f, sc->buf, (usize) want, sc->off, &got);
    if (rv != 0) {
        return rv;
    }
    sc->win_off = sc->off;
    sc->win_len = got;
    if ((u64) got >= n) {
        *p = sc->buf;
    }
    return 0;
}

/* Open the segment and read its header. The sequence number must match the filename. */
static int scan_open(
    const ps_vfs *vfs,
    const char *path,
    seg_scan *sc,
    u64 seq,
    ps_wal_seg_header *h)
{
    scan_close(vfs, sc);
    sc->win_len = 0;
    int rv = vfs->open(vfs->ctx, path, 0, &sc->f);
    if (rv != 0) {
        return rv;
    }
    sc->have_f = true;
    rv = vfs->size(vfs->ctx, &sc->f, &sc->size);
    if (rv != 0) {
        return rv;
    }
    sc->off = 0;
    const u8 *p = NULL;
    rv = scan_fill(vfs, sc, PS_WAL_SEG_HEADER_SIZE, &p);
    if (rv != 0) {
        return rv;
    }
    if (p == NULL || ps_wal_seg_header_decode(p, h) != 0 || h->seq != seq) {
        return EBADMSG;
    }
    sc->off = PS_WAL_SEG_HEADER_SIZE;
    sc->last = h->prev_lsn;
    return 0;
}

/* If the next record is invalid or incomplete, return 0 with *ok false and keep the offset. */
static int scan_next(
    const ps_vfs *vfs,
    seg_scan *sc,
    ps_lsn *lsn,
    const u8 **payload,
    u32 *len,
    bool *ok)
{
    *ok = false;
    const u8 *p = NULL;
    int rv = scan_fill(vfs, sc, PS_WAL_REC_HEADER_SIZE, &p);
    if (rv != 0 || p == NULL) {
        return rv;
    }
    const u64 size = (u64) PS_WAL_REC_HEADER_SIZE + (((u64) ps_get_le32(p) + 7) & ~(u64) 7);
    rv = scan_fill(vfs, sc, size, &p);
    if (rv != 0 || p == NULL) {
        return rv;
    }
    usize rsize = 0;
    if (ps_wal_rec_decode(p, (usize) size, sc->last, lsn, payload, len, &rsize) != 0) {
        return 0;
    }
    sc->off += rsize;
    sc->last = *lsn;
    *ok = true;
    return 0;
}

/* ---- open ---------------------------------------------------------------- */

static int cmp_u64(const void *a, const void *b)
{
    const u64 x = *(const u64 *) a;
    const u64 y = *(const u64 *) b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* An empty WAL starts at segment 1 when the store's durable LSN is PS_LSN_NONE. */
static int open_fresh(ps_wal *w)
{
    if (!ps_lsn_is_none(ps_durable_lsn(w->store))) {
        PS_LOG_ERROR(
            &w->logger,
            "WAL %s has no segments but the store's durable LSN is %llu: the log was lost",
            w->dir,
            ps_durable_lsn(w->store).raw);
        return EBADMSG;
    }
    int rv = reserve_seg(w);
    if (rv != 0) {
        return rv;
    }
    bool clean = false;
    rv = publish(w, 1, PS_LSN_NONE, &w->seg, &clean);
    if (rv != 0) {
        return rv;
    }
    w->have_seg = true;
    w->cur_seq = 1;
    w->first_seq = 1;
    w->prev_lsns[0] = PS_LSN_NONE;
    w->nsegs = 1;
    w->file_off = PS_WAL_SEG_HEADER_SIZE;
    w->seg_end = PS_WAL_SEG_HEADER_SIZE;
    return 0;
}

/* Work backward to the newest segment with prev_lsn <= d. Older files can have gaps. */
static int find_anchor(
    ps_wal *w,
    seg_scan *sc,
    const u64 *segs,
    usize n,
    ps_lsn d,
    ps_lsn *prev,
    usize *anchor)
{
    for (usize i = n; i-- > 0;) {
        ps_wal_seg_header h;
        int rv = scan_open(&w->vfs, seg_path(w, w->path, segs[i], false), sc, segs[i], &h);
        scan_close(&w->vfs, sc);
        if (rv == EBADMSG) {
            PS_LOG_ERROR(&w->logger, "WAL %s: segment %016llx has a bad header", w->dir, segs[i]);
        }
        if (rv != 0) {
            return rv;
        }
        prev[i] = h.prev_lsn;
        if (ps_lsn_le(h.prev_lsn, d)) {
            *anchor = i;
            return 0;
        }
        if (i == 0) {
            break;
        }
        if (segs[i - 1] != segs[i] - 1) {
            PS_LOG_ERROR(
                &w->logger,
                "WAL %s: segment %016llx is missing (records after the durable LSN %llu are lost)",
                w->dir,
                segs[i] - 1,
                d.raw);
            return EBADMSG;
        }
    }
    PS_LOG_ERROR(
        &w->logger,
        "WAL %s: no segment starts at or before the durable LSN %llu (oldest %016llx starts after %llu)",
        w->dir,
        d.raw,
        segs[0],
        prev[0].raw);
    return EBADMSG;
}

/*
 * Only the newest file may stop parsing before EOF. For older segments
 * the next header also confirms the LSN at the end of the file.
 */
static int validate(
    ps_wal *w,
    seg_scan *sc,
    const u64 *segs,
    usize n,
    usize anchor,
    const ps_lsn *prev,
    ps_lsn *last,
    u64 *end,
    u64 *size)
{
    for (usize i = anchor; i < n; i++) {
        if (i > anchor && !ps_lsn_eq(prev[i], last[i - 1])) {
            PS_LOG_ERROR(
                &w->logger,
                "WAL %s: segment %016llx starts after LSN %llu but segment %016llx ends at LSN %llu",
                w->dir,
                segs[i],
                prev[i].raw,
                segs[i - 1],
                last[i - 1].raw);
            return EBADMSG;
        }
        ps_wal_seg_header h;
        int rv = scan_open(&w->vfs, seg_path(w, w->path, segs[i], false), sc, segs[i], &h);
        if (rv == 0 && !ps_lsn_eq(h.prev_lsn, prev[i])) {
            rv = EBADMSG; /* The header changed after find_anchor read it. */
        }
        while (rv == 0) {
            ps_lsn lsn = PS_LSN_NONE;
            const u8 *payload = NULL;
            u32 len = 0;
            bool ok = false;
            rv = scan_next(&w->vfs, sc, &lsn, &payload, &len, &ok);
            if (!ok) {
                break;
            }
        }
        const u64 off = sc->off;
        const u64 sz = sc->size;
        last[i] = sc->last;
        scan_close(&w->vfs, sc);
        if (rv == EBADMSG) {
            PS_LOG_ERROR(&w->logger, "WAL %s: segment %016llx has a bad header", w->dir, segs[i]);
        }
        if (rv != 0) {
            return rv;
        }
        if (i + 1 < n && off != sz) {
            PS_LOG_ERROR(
                &w->logger,
                "WAL %s: sealed segment %016llx has an invalid record at offset %llu (size %llu)",
                w->dir,
                segs[i],
                off,
                sz);
            return EBADMSG;
        }
        *end = off;
        *size = sz;
    }
    return 0;
}

/* Sync even at the right size. A prior rotation may have failed after truncating. */
static int seal_at_open(ps_wal *w, u64 seq, u64 end, u64 size)
{
    ps_file f;
    int rv = w->vfs.open(w->vfs.ctx, seg_path(w, w->path, seq, false), 0, &f);
    if (rv == 0) {
        if (size != end) {
            rv = w->vfs.truncate(w->vfs.ctx, &f, end);
        }
        if (rv == 0) {
            rv = w->vfs.fsync(w->vfs.ctx, &f);
        }
        w->vfs.close(w->vfs.ctx, &f);
    }
    if (rv != 0) {
        char buf[128];
        PS_LOG_ERROR(
            &w->logger,
            "WAL %s: sealing segment %016llx failed: %s (error %d)",
            w->dir,
            seq,
            ps_strerror(rv, buf, sizeof(buf)),
            rv);
    }
    return rv;
}

/*
 * Validate the retained suffix before changing any of its files. Once the
 * newest segment is sealed, either it remains newest after a crash or a
 * durable successor points to it. In both cases the next open can parse
 * that sealed segment to EOF.
 */
static int open_existing(ps_wal *w, u64 *segs, usize n)
{
    qsort(segs, n, sizeof(*segs), cmp_u64);
    const ps_lsn d = ps_durable_lsn(w->store);
    ps_lsn *prev = calloc(n, sizeof(*prev));
    ps_lsn *last = calloc(n, sizeof(*last));
    seg_scan sc;
    memset(&sc, 0, sizeof(sc));
    int rv = prev == NULL || last == NULL ? ENOMEM : 0;
    usize anchor = 0;
    u64 end = 0;
    u64 size = 0;
    if (rv == 0) {
        rv = find_anchor(w, &sc, segs, n, d, prev, &anchor);
    }
    if (rv == 0) {
        rv = validate(w, &sc, segs, n, anchor, prev, last, &end, &size);
    }
    scan_free(&w->vfs, &sc);
    const u64 newest = segs[n - 1];
    if (rv == 0 && newest == UINT64_MAX) {
        PS_LOG_ERROR(&w->logger, "WAL %s: segment sequence numbers exhausted", w->dir);
        rv = EBADMSG;
    }
    /* Allocate before touching retained segments so ENOMEM here leaves them alone. */
    const usize live = n - anchor;
    if (rv == 0) {
        const usize cap = PS_MAX(live + 1, (usize) 8);
        w->prev_lsns = malloc(cap * sizeof(*w->prev_lsns));
        if (w->prev_lsns == NULL) {
            rv = ENOMEM;
        } else {
            w->segs_cap = cap;
        }
    }
    for (usize i = 0; rv == 0 && i < anchor; i++) {
        rv = w->vfs.unlink(w->vfs.ctx, seg_path(w, w->path, segs[i], false));
        if (rv == ENOENT) {
            rv = 0;
        }
    }
    if (rv == 0) {
        rv = seal_at_open(w, newest, end, size);
    }
    const ps_lsn last_lsn = rv == 0 ? last[n - 1] : PS_LSN_NONE;
    if (rv == 0 && ps_lsn_lt(last_lsn, d)) {
        PS_LOG_WARN(
            &w->logger,
            "WAL %s: the last valid LSN %llu is below the store's durable LSN %llu "
            "(operations not logged, or the newest segments were lost)",
            w->dir,
            last_lsn.raw,
            d.raw);
    }
    if (rv == 0) {
        bool clean = false;
        rv = publish(w, newest + 1, last_lsn, &w->seg, &clean);
    }
    if (rv == 0) {
        w->have_seg = true;
        w->first_seq = segs[anchor];
        w->cur_seq = newest + 1;
        for (usize i = 0; i < live; i++) {
            w->prev_lsns[i] = prev[anchor + i];
        }
        w->prev_lsns[live] = last_lsn;
        w->nsegs = live + 1;
        w->file_off = PS_WAL_SEG_HEADER_SIZE;
        w->seg_end = PS_WAL_SEG_HEADER_SIZE;
        w->written = last_lsn;
        w->synced = last_lsn;
        w->appended = last_lsn;
        PS_LOG_INFO(
            &w->logger,
            "WAL %s opened: segments %016llx..%016llx, last LSN %llu, durable LSN %llu",
            w->dir,
            segs[anchor],
            newest,
            last_lsn.raw,
            d.raw);
    }
    free(prev);
    free(last);
    return rv;
}

static int open_segments(ps_wal *w)
{
    ps_vfs_list_ctx lc;
    memset(&lc, 0, sizeof(lc));
    int rv = w->vfs.list(w->vfs.ctx, w->dir, list_entry, &lc);
    if (rv == 0) {
        /* Only published names belong to the log, even if a temp file has a valid header. */
        for (usize i = 0; i < lc.ntmps && rv == 0; i++) {
            rv = w->vfs.unlink(w->vfs.ctx, seg_path(w, w->path, lc.tmps[i], true));
            if (rv == ENOENT) {
                rv = 0;
            }
        }
    }
    if (rv == 0) {
        rv = lc.nsegs > 0 ? open_existing(w, lc.segs, lc.nsegs) : open_fresh(w);
    }
    free(lc.tmps);
    free(lc.segs);
    return rv;
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
        PS_LOG_ERROR(ps_store_logger(store), "WAL config out of range");
        return EINVAL;
    }
    if (waldir == NULL && ps_store_dir(store) == NULL) {
        PS_LOG_ERROR(
            ps_store_logger(store),
            "WAL: a NULL waldir needs a store opened with ps_open");
        return EINVAL;
    }

    ps_wal *w = calloc(1, sizeof(*w));
    if (w == NULL) {
        return ENOMEM;
    }
    w->store = store;
    w->vfs = *ps_store_vfs(store);
    w->logger = *ps_store_logger(store);
    w->cfg = c;
    w->hctx.w = w;
    atomic_init(&w->failed, false);
    atomic_init(&w->appending, false);
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

ps_store *ps_wal_store(const ps_wal *w)
{
    return w->store;
}

/* ---- iterator ------------------------------------------------------------ */

struct ps_wal_iter {
    ps_wal *w;
    ps_lsn from;
    u64 first_seq;
    u64 seq; /* the current or next segment to read */
    u64 end_seq; /* new segment, empty until the first append */
    ps_lsn *prevs; /* copy of prev_lsn for segments first_seq..end_seq */
    char *path;
    seg_scan sc;
    bool done;
};

static bool appending(ps_wal *w)
{
    return atomic_load_explicit(&w->appending, memory_order_acquire);
}

int ps_wal_iter_open(ps_wal *w, ps_lsn from_lsn, ps_wal_iter **out)
{
    if (out != NULL) {
        *out = NULL;
    }
    if (w == NULL || out == NULL) {
        return EINVAL;
    }
    if (appending(w)) {
        PS_LOG_ERROR(&w->logger, "ps_wal_iter_open: the WAL was appended to since open");
        return EINVAL;
    }
    ps_wal_iter *it = calloc(1, sizeof(*it));
    if (it == NULL) {
        return ENOMEM;
    }
    it->w = w;
    it->from = from_lsn;
    it->path = malloc(w->dir_len + WAL_NAME_LEN + 2);
    if (it->path == NULL) {
        free(it);
        return ENOMEM;
    }
    memcpy(it->path, w->dir, w->dir_len + 1);
    pthread_mutex_lock(&w->io_mu);
    const usize n = w->nsegs;
    it->prevs = malloc(n * sizeof(*it->prevs));
    if (it->prevs != NULL) {
        memcpy(it->prevs, w->prev_lsns, n * sizeof(*it->prevs));
        it->first_seq = w->first_seq;
        it->seq = w->first_seq;
        it->end_seq = w->cur_seq;
        w->iters++;
    }
    pthread_mutex_unlock(&w->io_mu);
    if (it->prevs == NULL) {
        free(it->path);
        free(it);
        return ENOMEM;
    }
    *out = it;
    return 0;
}

static int iter_changed(ps_wal_iter *it, const char *what)
{
    PS_LOG_ERROR(
        &it->w->logger,
        "WAL %s: segment %016llx %s since open",
        it->w->dir,
        it->seq,
        what);
    return EBADMSG;
}

int ps_wal_iter_next(ps_wal_iter *it, ps_lsn *lsn, const void **buf, usize *len)
{
    ps_wal *w = it->w;
    if (appending(w)) {
        PS_LOG_ERROR(&w->logger, "ps_wal_iter_next: the WAL was appended to since open");
        return EINVAL;
    }
    while (!it->done) {
        const usize idx = (usize) (it->seq - it->first_seq);
        if (!it->sc.have_f) {
            if (it->seq >= it->end_seq) {
                it->done = true;
                break;
            }
            /* The next header tells us whether we can skip this whole file. */
            if (ps_lsn_lt(it->prevs[idx + 1], it->from)) {
                it->seq++;
                continue;
            }
            ps_wal_seg_header h;
            const int rv =
                scan_open(&w->vfs, seg_path(w, it->path, it->seq, false), &it->sc, it->seq, &h);
            if (rv == ENOENT) {
                scan_close(&w->vfs, &it->sc);
                return iter_changed(it, "was deleted");
            }
            if (rv == EBADMSG || (rv == 0 && !ps_lsn_eq(h.prev_lsn, it->prevs[idx]))) {
                scan_close(&w->vfs, &it->sc);
                return iter_changed(it, "has a different header");
            }
            if (rv != 0) {
                scan_close(&w->vfs, &it->sc);
                return rv;
            }
        }
        ps_lsn l = PS_LSN_NONE;
        const u8 *payload = NULL;
        u32 n = 0;
        bool ok = false;
        const int rv = scan_next(&w->vfs, &it->sc, &l, &payload, &n, &ok);
        if (rv != 0) {
            return rv;
        }
        if (!ok) {
            /* This segment is sealed. Expect EOF at the next header's prev_lsn. */
            if (it->sc.off != it->sc.size || !ps_lsn_eq(it->sc.last, it->prevs[idx + 1])) {
                return iter_changed(it, "changed");
            }
            scan_close(&w->vfs, &it->sc);
            it->seq++;
            continue;
        }
        if (ps_lsn_lt(l, it->from)) {
            continue;
        }
        *lsn = l;
        *buf = payload;
        *len = n;
        return 0;
    }
    return ENOENT;
}

void ps_wal_iter_close(ps_wal_iter *it)
{
    if (it == NULL) {
        return;
    }
    ps_wal *w = it->w;
    pthread_mutex_lock(&w->io_mu);
    w->iters--;
    pthread_mutex_unlock(&w->io_mu);
    scan_free(&w->vfs, &it->sc);
    free(it->prevs);
    free(it->path);
    free(it);
}
