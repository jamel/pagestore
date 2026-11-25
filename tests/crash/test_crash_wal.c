/*
 * Scan the surviving WAL before opening the store. Recovery must end
 * at that LSN and match the model at that point, even if recovery itself
 * crashes and has to start over.
 */

#include "crc32c.h"
#include "fault_vfs.h"
#include "format.h"
#include "model.h"
#include "rng.h"
#include "test.h"
#include "workload.h"

#include "pagestore/bytes.h"
#include "pagestore/pagestore.h"
#include "pagestore/vfs.h"
#include "pagestore/wal.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PS 4096u
#define DIR "/s"
#define WALDIR "/s/wal"
#define FRAMES 64u
#define SEG ((u64) 64 * 1024)
#define NOPS 300u
#define REC_HEAD 8u
#define PAD_MAX 4000u
#define REC_MAX (REC_HEAD + W_RECORD_MAX + PAD_MAX)
#define NEXT_ALLOCS 4u
#define NCLASSES 4u

/* ---- WAL fault injection ---- */

#define WRAP_FILES 32u

typedef struct wrap_file {
    bool used;
    bool truncated; /* An fsync after truncate is part of sealing. */
    ps_file f;
    char path[64];
} wrap_file;

/*
 * Counts WAL rotation, truncation and open apart from appends and flushes,
 * so crashes can target segment changes in a write-heavy workload.
 */
struct ps_vfs_ctx {
    ps_vfs inner;
    fault_vfs *fv;
    pthread_mutex_t mu;
    bool in_open;
    u64 target; /* 0 = never */
    u64 calls;
    u64 unlinks; /* segment unlinks (truncation or obsolete at open) */
    u64 renames;
    /* Slows the checkpoint so the writer rotates ahead of truncation. */
    u32 journal_sleep_us;
    wrap_file files[WRAP_FILES];
};

static bool is_wal_path(const char *p)
{
    return strncmp(p, WALDIR "/", sizeof(WALDIR)) == 0;
}

static bool is_tmp_path(const char *p)
{
    const usize n = strlen(p);
    return n > 4 && strcmp(p + n - 4, ".tmp") == 0;
}

/* Hold mu. The next mutating VFS call consumes the armed point. */
static void wrap_note(ps_vfs_ctx *v, bool interesting)
{
    if (!interesting) {
        return;
    }
    v->calls++;
    if (v->target != 0 && v->calls == v->target) {
        fault_vfs_crash_at(v->fv, fault_vfs_points(v->fv) + 1);
    }
}

static wrap_file *wrap_find(ps_vfs_ctx *v, const ps_file *f)
{
    for (u32 i = 0; i < WRAP_FILES; i++) {
        if (v->files[i].used && memcmp(&v->files[i].f, f, sizeof(*f)) == 0) {
            return &v->files[i];
        }
    }
    return NULL;
}

static int wrap_open(ps_vfs_ctx *v, const char *path, u32 flags, ps_file *out)
{
    pthread_mutex_lock(&v->mu);
    wrap_note(v, is_wal_path(path) && is_tmp_path(path) && (flags & PS_VFS_CREATE) != 0);
    pthread_mutex_unlock(&v->mu);
    const int rv = v->inner.open(v->inner.ctx, path, flags, out);
    if (rv != 0) {
        return rv;
    }
    pthread_mutex_lock(&v->mu);
    wrap_file *e = NULL;
    for (u32 i = 0; i < WRAP_FILES && e == NULL; i++) {
        if (!v->files[i].used) {
            e = &v->files[i];
        }
    }
    T_CHECK(e != NULL && strlen(path) < sizeof(e->path));
    e->used = true;
    e->truncated = false;
    e->f = *out;
    strcpy(e->path, path);
    pthread_mutex_unlock(&v->mu);
    return 0;
}

static void wrap_close(ps_vfs_ctx *v, ps_file *f)
{
    pthread_mutex_lock(&v->mu);
    wrap_file *e = wrap_find(v, f);
    if (e != NULL) {
        e->used = false;
    }
    pthread_mutex_unlock(&v->mu);
    v->inner.close(v->inner.ctx, f);
}

static int wrap_pread(ps_vfs_ctx *v, ps_file *f, void *buf, usize len, u64 off, usize *got)
{
    return v->inner.pread(v->inner.ctx, f, buf, len, off, got);
}

typedef enum wrap_op {
    WRAP_PWRITE,
    WRAP_FSYNC,
    WRAP_TRUNCATE,
    WRAP_PREALLOCATE
} wrap_op;

/* Counts a call on an open file. */
static void wrap_file_call(ps_vfs_ctx *v, ps_file *f, wrap_op op)
{
    pthread_mutex_lock(&v->mu);
    wrap_file *e = wrap_find(v, f);
    bool interesting = false;
    if (e != NULL && is_wal_path(e->path)) {
        const bool tmp = is_tmp_path(e->path);
        switch (op) {
            case WRAP_PWRITE:
                interesting = tmp || v->in_open;
                break;
            case WRAP_FSYNC:
                interesting = tmp || e->truncated || v->in_open;
                e->truncated = false;
                break;
            case WRAP_TRUNCATE:
                interesting = true;
                e->truncated = true;
                break;
            case WRAP_PREALLOCATE:
                interesting = true;
                break;
        }
    }
    wrap_note(v, interesting);
    pthread_mutex_unlock(&v->mu);
}

static int wrap_pwrite(ps_vfs_ctx *v, ps_file *f, const void *buf, usize len, u64 off)
{
    wrap_file_call(v, f, WRAP_PWRITE);
    return v->inner.pwrite(v->inner.ctx, f, buf, len, off);
}

static int wrap_fsync(ps_vfs_ctx *v, ps_file *f)
{
    wrap_file_call(v, f, WRAP_FSYNC);
    if (v->journal_sleep_us != 0) {
        pthread_mutex_lock(&v->mu);
        const wrap_file *e = wrap_find(v, f);
        const bool journal = e != NULL && strcmp(e->path, DIR "/journal") == 0;
        pthread_mutex_unlock(&v->mu);
        if (journal) {
            t_sleep_us(v->journal_sleep_us);
        }
    }
    return v->inner.fsync(v->inner.ctx, f);
}

static int wrap_size(ps_vfs_ctx *v, ps_file *f, u64 *out)
{
    return v->inner.size(v->inner.ctx, f, out);
}

static int wrap_truncate(ps_vfs_ctx *v, ps_file *f, u64 size)
{
    wrap_file_call(v, f, WRAP_TRUNCATE);
    return v->inner.truncate(v->inner.ctx, f, size);
}

static int wrap_preallocate(ps_vfs_ctx *v, ps_file *f, u64 size)
{
    wrap_file_call(v, f, WRAP_PREALLOCATE);
    return v->inner.preallocate(v->inner.ctx, f, size);
}

static int wrap_unlink(ps_vfs_ctx *v, const char *path)
{
    pthread_mutex_lock(&v->mu);
    wrap_note(v, is_wal_path(path));
    if (is_wal_path(path) && !is_tmp_path(path)) {
        v->unlinks++;
    }
    pthread_mutex_unlock(&v->mu);
    return v->inner.unlink(v->inner.ctx, path);
}

static int wrap_rename(ps_vfs_ctx *v, const char *from, const char *to)
{
    pthread_mutex_lock(&v->mu);
    wrap_note(v, is_wal_path(from));
    if (is_wal_path(from)) {
        v->renames++;
    }
    pthread_mutex_unlock(&v->mu);
    const int rv = v->inner.rename(v->inner.ctx, from, to);
    if (rv == 0) {
        /* Later appends through this handle count as ordinary record writes. */
        pthread_mutex_lock(&v->mu);
        for (u32 i = 0; i < WRAP_FILES; i++) {
            wrap_file *e = &v->files[i];
            if (e->used && strcmp(e->path, from) == 0) {
                T_CHECK(strlen(to) < sizeof(e->path));
                strcpy(e->path, to);
            }
        }
        pthread_mutex_unlock(&v->mu);
    }
    return rv;
}

static int wrap_mkdir(ps_vfs_ctx *v, const char *path)
{
    pthread_mutex_lock(&v->mu);
    wrap_note(v, v->in_open && strcmp(path, WALDIR) == 0);
    pthread_mutex_unlock(&v->mu);
    return v->inner.mkdir(v->inner.ctx, path);
}

static int wrap_fsync_dir(ps_vfs_ctx *v, const char *path)
{
    pthread_mutex_lock(&v->mu);
    wrap_note(v, strcmp(path, WALDIR) == 0 || (v->in_open && strcmp(path, DIR) == 0));
    pthread_mutex_unlock(&v->mu);
    return v->inner.fsync_dir(v->inner.ctx, path);
}

static int wrap_list(ps_vfs_ctx *v, const char *dir, ps_vfs_list_fn fn, ps_vfs_list_ctx *arg)
{
    return v->inner.list(v->inner.ctx, dir, fn, arg);
}

static int wrap_lock(ps_vfs_ctx *v, const char *path, ps_file *out)
{
    return v->inner.lock(v->inner.ctx, path, out);
}

static void wrap_set_in_open(ps_vfs_ctx *v, bool on)
{
    pthread_mutex_lock(&v->mu);
    v->in_open = on;
    pthread_mutex_unlock(&v->mu);
}

/* Restart the targeted call count. Pass 0 to disarm. */
static void wrap_arm(ps_vfs_ctx *v, u64 target)
{
    pthread_mutex_lock(&v->mu);
    v->calls = 0;
    v->target = target;
    pthread_mutex_unlock(&v->mu);
}

static u64 wrap_calls(ps_vfs_ctx *v)
{
    pthread_mutex_lock(&v->mu);
    const u64 n = v->calls;
    pthread_mutex_unlock(&v->mu);
    return n;
}

/* ---- test fixture ---- */

typedef struct wcase {
    ps_vfs_ctx vc;
    ps_vfs vfs;
    fault_vfs *fv;
    ps_wal_config wcfg;
    ps_store *s;
    ps_wal *w;
    model *m;
    rng r; /* Same workload seed for every case in the class. */
    rng r2; /* per-case decisions (ENOSPC injection, recovery crash points) */
    u32 pad_max;
    u32 flush_pct;
    bool enospc_run;
    u32 enospc_injected;
    u64 max_flushed; /* Lower bound for the next recovery. */
    u64 seg_end; /* predicted end of the WAL's newest segment */
    bool sealed; /* a failed rotation left the newest segment sealed */
    u64 *exp; /* Replay CRC by LSN. Bit 32 distinguishes a zero CRC from no record. */
    u64 nexp;
    u64 discarded; /* model operations above some recovered X */
    char desc[160];
} wcase;

#define CASE_CHECK(c_, cond) \
    do { \
        if (!(cond)) { \
            T_FAIL("[%s] CHECK(%s) failed", (c_)->desc, #cond); \
        } \
    } while (0)

#define CASE_OK(c_, e) \
    do { \
        const int case_rv_ = (e); \
        if (case_rv_ != 0) { \
            T_FAIL("[%s] %s returned %d", (c_)->desc, #e, case_rv_); \
        } \
    } while (0)

/* Workload classes: padding, flush share, append buffer. */
static void class_params(wcase *c, u32 cls)
{
    static const u32 pad[NCLASSES] = {PAD_MAX, 400, PAD_MAX, 1500};
    static const u32 flush[NCLASSES] = {100, 70, 30, 70};
    static const usize buffer[NCLASSES] = {1024 * 1024, 4096, 0, 1024 * 1024};
    static const u32 sleep_us[NCLASSES] = {0, 0, 300, 300};
    c->pad_max = pad[cls % NCLASSES];
    c->flush_pct = flush[cls % NCLASSES];
    c->wcfg.buffer_size = buffer[cls % NCLASSES];
    c->vc.journal_sleep_us = sleep_us[cls % NCLASSES];
}

static ps_config store_config(wcase *c)
{
    ps_config cfg;
    ps_config_init(&cfg);
    cfg.page_size = PS;
    cfg.cache_size = (usize) FRAMES * PS;
    cfg.checkpoint_thread = true;
    cfg.checkpoint_interval_ms = 2;
    cfg.vfs = &c->vfs;
    return cfg;
}

static void exp_set(wcase *c, u64 lsn, const u8 *rec, usize len)
{
    if (lsn >= c->nexp) {
        u64 n = c->nexp == 0 ? 1024 : c->nexp;
        while (n <= lsn) {
            n *= 2;
        }
        u64 *e = t_realloc(c->exp, n * sizeof(*e));
        memset(e + c->nexp, 0, (n - c->nexp) * sizeof(*e));
        c->exp = e;
        c->nexp = n;
    }
    c->exp[lsn] = ((u64) 1 << 32) | ps_crc32c(0, rec, len);
}

static void exp_truncate(wcase *c, u64 lsn)
{
    for (u64 i = lsn + 1; i < c->nexp; i++) {
        c->exp[i] = 0;
    }
}

struct ps_apply_ctx {
    wcase *c;
    u64 last;
};

/* Check the replayed record against the CRC saved at append time. */
static int apply_rec(ps_apply_ctx *a, ps_store *s, ps_lsn at, const void *rec, usize len)
{
    wcase *c = a->c;
    const u64 lsn = at.raw;
    CASE_CHECK(c, lsn > a->last);
    a->last = lsn;
    const bool known = lsn < c->nexp && c->exp[lsn] != 0;
    if (!known || c->exp[lsn] != (((u64) 1 << 32) | ps_crc32c(0, rec, len))) {
        T_FAIL(
            "[%s] replayed record %llu was never appended %s",
            c->desc,
            (unsigned long long) lsn,
            known ? "with these bytes" : "(or was discarded)");
    }
    const u8 *b = rec;
    CASE_CHECK(c, len >= REC_HEAD);
    const u32 enc = ps_get_le32(b);
    const u32 pad = ps_get_le32(b + 4);
    CASE_CHECK(c, (usize) REC_HEAD + enc + pad == len);
    for (u32 i = 0; i < pad; i++) {
        CASE_CHECK(c, b[REC_HEAD + enc + i] == w_pattern(lsn ^ 0xA5A5u, i));
    }
    return w_apply_record(a, s, at, b + REC_HEAD, enc);
}

static usize encode_rec(wcase *c, const w_op *op, u8 *rec)
{
    const usize enc = w_encode(op, rec + REC_HEAD, W_RECORD_MAX);
    T_CHECK(enc <= W_RECORD_MAX);
    /* Padding makes short runs cross segment boundaries. */
    const u32 pad = c->pad_max == 0 ? 0 : (u32) rng_below(&c->r, (u64) c->pad_max + 1);
    ps_put_le32(rec, (u32) enc);
    ps_put_le32(rec + 4, pad);
    w_pattern_fill(op->lsn ^ 0xA5A5u, 0, rec + REC_HEAD + enc, pad);
    return REC_HEAD + enc + pad;
}

static void shutdown_both(wcase *c)
{
    if (c->s != NULL) {
        ps_close(c->s);
        c->s = NULL;
    }
    if (c->w != NULL) {
        ps_wal_close(c->w);
        c->w = NULL;
    }
}

/* On failure, both handles are closed so the case can reboot and retry. */
static int startup(wcase *c, u32 flags)
{
    const ps_config cfg = store_config(c);
    int rv = ps_open(DIR, &cfg, flags, &c->s);
    if (rv != 0) {
        c->s = NULL;
        return rv;
    }
    wrap_set_in_open(&c->vc, true);
    rv = ps_wal_open(c->s, NULL, &c->wcfg, &c->w);
    wrap_set_in_open(&c->vc, false);
    if (rv != 0) {
        c->w = NULL;
        shutdown_both(c);
        return rv;
    }
    ps_apply_ctx a = {.c = c, .last = ps_durable_lsn(c->s).raw};
    rv = ps_recover(c->s, c->w, apply_rec, &a);
    if (rv != 0) {
        shutdown_both(c);
        return rv;
    }
    c->seg_end = PS_WAL_SEG_HEADER_SIZE;
    c->sealed = false;
    return 0;
}

static wcase *case_new(u32 cls, u64 seed)
{
    wcase *c = t_calloc(1, sizeof(*c));
    c->fv = fault_vfs_create(seed);
    c->vc.inner = *fault_vfs_get(c->fv);
    c->vc.fv = c->fv;
    T_OK(pthread_mutex_init(&c->vc.mu, NULL));
    c->vfs = (ps_vfs) {
        .ctx = &c->vc,
        .open = wrap_open,
        .close = wrap_close,
        .pread = wrap_pread,
        .pwrite = wrap_pwrite,
        .fsync = wrap_fsync,
        .size = wrap_size,
        .truncate = wrap_truncate,
        .preallocate = wrap_preallocate,
        .unlink = wrap_unlink,
        .rename = wrap_rename,
        .mkdir = wrap_mkdir,
        .fsync_dir = wrap_fsync_dir,
        .list = wrap_list,
        .lock = wrap_lock,
    };
    ps_wal_config_init(&c->wcfg);
    c->wcfg.segment_size = SEG;
    c->wcfg.max_record_size = REC_MAX;
    class_params(c, cls);
    c->m = model_create(PS);
    rng_seed(&c->r, 0xC1A55u + cls);
    rng_seed(&c->r2, seed);
    c->enospc_run = rng_below(&c->r2, 10) == 0;
    snprintf(c->desc, sizeof(c->desc), "class %u seed %llu", cls, (unsigned long long) seed);
    CASE_OK(c, startup(c, PS_CREATE));
    return c;
}

static void case_free(wcase *c)
{
    shutdown_both(c);
    model_destroy(c->m);
    fault_vfs_destroy(c->fv);
    pthread_mutex_destroy(&c->vc.mu);
    free(c->exp);
    free(c);
}

/* A failure of a store or WAL call is only allowed after a crash or a full disk. */
static void expect_stop(wcase *c, int rv, const char *what)
{
    if (!fault_vfs_crashed(c->fv) && c->enospc_injected == 0) {
        T_FAIL("[%s] %s failed (%d) without a crash", c->desc, what, rv);
    }
}

/* Returns false if a crash or full disk stops the workload early. */
static bool workload(wcase *c, u32 nops)
{
    u8 *rec = t_malloc(REC_MAX);
    CASE_CHECK(c, ps_committed_lsn(c->s).raw == model_last_lsn(c->m));
    u64 lsn = ps_committed_lsn(c->s).raw;
    u64 retry = 0;
    bool done = true;
    for (u32 i = 0; i < nops; i++) {
        u64 next = lsn + 1;
        if (retry != 0) {
            next = retry;
            retry = 0;
        } else if (rng_below(&c->r, 10) == 0) {
            next += 1 + rng_below(&c->r, 3);
        }
        w_op op;
        w_gen(&c->r, c->m, next, W_MAX_ACTIONS, &op);
        int rv = w_apply_store_begin(c->s, &op);
        if (rv != 0) {
            expect_stop(c, rv, "operation");
            done = false;
            break;
        }
        const usize len = encode_rec(c, &op, rec);
        const usize size = ps_wal_rec_size((u32) len);
        const bool rotates = c->sealed || c->seg_end + size > SEG;
        const bool inject =
            rotates && c->enospc_run && (c->enospc_injected == 0 || rng_below(&c->r2, 2) == 0);
        if (inject) {
            c->enospc_injected++;
            fault_vfs_enospc_after(c->fv, 0);
        }
        rv = ps_wal_append(c->w, ps_lsn_from(next), rec, len);
        if (inject) {
            fault_vfs_enospc_after(c->fv, UINT64_MAX);
        }
        if (rv == ENOSPC) {
            /* A failed rotation can seal the old segment without appending. */
            CASE_CHECK(c, inject);
            ps_op_abort(c->s);
            c->sealed = true;
            retry = next;
            continue;
        }
        if (rv != 0) {
            CASE_CHECK(c, rv == EIO);
            expect_stop(c, rv, "ps_wal_append");
            ps_op_abort(c->s);
            /* Record the attempted append so the model can accept it if it survives. */
            exp_set(c, next, rec, len);
            w_apply_model(c->m, &op);
            done = false;
            break;
        }
        CASE_CHECK(c, !inject || fault_vfs_crashed(c->fv));
        if (rotates) {
            c->seg_end = PS_WAL_SEG_HEADER_SIZE;
            c->sealed = false;
        }
        c->seg_end += size;
        exp_set(c, next, rec, len);
        w_apply_model(c->m, &op);
        if (rng_below(&c->r, 100) < c->flush_pct) {
            rv = ps_wal_flush(c->w, ps_lsn_from(next));
            if (rv != 0) {
                CASE_CHECK(c, rv == EIO);
                expect_stop(c, rv, "ps_wal_flush");
                ps_op_abort(c->s);
                done = false;
                break;
            }
            c->max_flushed = next;
        }
        ps_op_commit(c->s);
        lsn = next;
        if (rng_below(&c->r, 100) < 3) {
            rv = ps_checkpoint(c->s);
            if (rv != 0) {
                expect_stop(c, rv, "ps_checkpoint");
            }
        }
    }
    free(rec);
    return done;
}

/* ---- recovery checks ---- */

struct ps_vfs_list_ctx {
    u64 newest;
};

static int list_seg(ps_vfs_list_ctx *lc, const char *name)
{
    if (strlen(name) != 20 || strcmp(name + 16, ".wal") != 0) {
        return 0;
    }
    u64 seq = 0;
    for (u32 i = 0; i < 16; i++) {
        const char ch = name[i];
        u32 d = 0;
        if (ch >= '0' && ch <= '9') {
            d = (u32) (ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            d = (u32) (ch - 'a') + 10;
        } else {
            return 0;
        }
        seq = seq * 16 + d;
    }
    if (seq > lc->newest) {
        lc->newest = seq;
    }
    return 0;
}

/*
 * Read the newest segment without ps_wal_open. An empty segment's
 * prev_lsn is the last record from the preceding segment.
 */
static u64 scan_last_lsn(wcase *c)
{
    const ps_vfs *fvfs = fault_vfs_get(c->fv);
    ps_vfs_list_ctx lc = {0};
    CASE_OK(c, fvfs->list(fvfs->ctx, WALDIR, list_seg, &lc));
    CASE_CHECK(c, lc.newest != 0);
    char path[64];
    snprintf(path, sizeof(path), WALDIR "/%016llx.wal", (unsigned long long) lc.newest);
    u8 *buf = t_malloc(SEG);
    usize got = 0;
    CASE_OK(c, fault_vfs_read_durable(c->fv, path, 0, buf, SEG, &got));
    ps_wal_seg_header h;
    CASE_CHECK(c, got >= PS_WAL_SEG_HEADER_SIZE);
    CASE_OK(c, ps_wal_seg_header_decode(buf, &h));
    CASE_CHECK(c, h.seq == lc.newest);
    u64 lsn = h.prev_lsn.raw;
    usize off = PS_WAL_SEG_HEADER_SIZE;
    while (off < got) {
        ps_lsn l = PS_LSN_NONE;
        const u8 *payload = NULL;
        u32 len = 0;
        usize size = 0;
        if (ps_wal_rec_decode(buf + off, got - off, ps_lsn_from(lsn), &l, &payload, &len, &size) !=
            0) {
            break;
        }
        lsn = l.raw;
        off += size;
    }
    free(buf);
    return lsn;
}

typedef enum rcrash {
    RC_NONE,
    RC_POINT, /* crash at the q-th VFS point of the startup sequence */
    RC_OPEN_CALL /* crash at the q-th interesting call of ps_wal_open */
} rcrash;

/* q is one-based. Returns true if startup reached the armed crash point. */
static bool crashed_startup(wcase *c, rcrash rc, u64 q, fault_mode mode)
{
    if (rc == RC_POINT) {
        fault_vfs_crash_at(c->fv, fault_vfs_points(c->fv) + q);
    } else {
        wrap_arm(&c->vc, q);
    }
    const int rv = startup(c, 0);
    const bool crashed = fault_vfs_crashed(c->fv);
    if (rv == 0) {
        shutdown_both(c);
    } else if (!crashed) {
        T_FAIL("[%s] startup failed (%d) without a crash", c->desc, rv);
    }
    wrap_arm(&c->vc, 0);
    fault_vfs_reboot(c->fv, mode);
    return crashed;
}

/*
 * Leave the recovered store and WAL open so another workload can
 * reuse LSNs above the surviving prefix.
 */
static u64 check(wcase *c, fault_mode mode, rcrash rc, u64 q, bool *rc_crashed)
{
    shutdown_both(c);
    wrap_arm(&c->vc, 0);
    fault_vfs_reboot(c->fv, mode);
    const u64 L = scan_last_lsn(c);
    bool crashed = false;
    if (rc != RC_NONE) {
        crashed =
            crashed_startup(c, rc, q, q % 2 == 0 ? FAULT_DROP_UNSYNCED : FAULT_RANDOM_UNSYNCED);
        CASE_CHECK(c, scan_last_lsn(c) == L);
    }
    if (rc_crashed != NULL) {
        *rc_crashed = crashed;
    }
    CASE_OK(c, startup(c, 0));
    const u64 X = ps_committed_lsn(c->s).raw;
    CASE_CHECK(c, X >= c->max_flushed);
    CASE_CHECK(c, X <= model_last_lsn(c->m));
    CASE_CHECK(c, X == ps_durable_lsn(c->s).raw || model_has_lsn(c->m, X));
    CASE_CHECK(c, model_has_lsn(c->m, X));
    CASE_CHECK(c, X == L);
    struct ps_stats st;
    ps_stats(c->s, &st);
    CASE_CHECK(c, !st.failed);

    if (X < model_last_lsn(c->m)) {
        c->discarded++;
    }
    model_truncate(c->m, X);
    exp_truncate(c, X);
    w_check_store(c->s, c->m);

    ps_page_id want[NEXT_ALLOCS];
    model_next_ids(c->m, NEXT_ALLOCS, want);
    CASE_OK(c, ps_op_begin(c->s, ps_lsn_from(X + 1)));
    for (u32 i = 0; i < NEXT_ALLOCS; i++) {
        ps_page_id id = PS_PAGE_ID_NONE;
        ps_page *p = NULL;
        CASE_OK(c, ps_alloc(c->s, &id, &p));
        CASE_CHECK(c, ps_page_id_eq(id, want[i]));
    }
    ps_op_abort(c->s);
    c->max_flushed = X;
    return X;
}

/* The store keeps working: more operations, a checkpoint, a power loss, a reopen. */
static void finish(wcase *c)
{
    CASE_CHECK(c, workload(c, 20));
    CASE_OK(c, ps_checkpoint(c->s));
    const u64 last = model_last_lsn(c->m);
    shutdown_both(c);
    fault_vfs_reboot(c->fv, FAULT_RANDOM_UNSYNCED);
    CASE_CHECK(c, scan_last_lsn(c) == last);
    CASE_OK(c, startup(c, 0));
    CASE_CHECK(c, ps_committed_lsn(c->s).raw == last);
    CASE_CHECK(c, ps_durable_lsn(c->s).raw == last);
    w_check_store(c->s, c->m);
}

/*
 * Use a crash-free run to size the search for fault points and check
 * that the workload reaches rotation and truncation.
 */
static u64 dry_points(u32 cls, u64 *calls)
{
    wcase *c = case_new(cls, 1000 + cls);
    c->enospc_run = false;
    wrap_arm(&c->vc, 0);
    const u64 base = fault_vfs_points(c->fv);
    CASE_CHECK(c, workload(c, NOPS));
    const u64 points = fault_vfs_points(c->fv) - base;
    if (calls != NULL) {
        *calls = wrap_calls(&c->vc);
    }
    pthread_mutex_lock(&c->vc.mu);
    const u64 renames = c->vc.renames;
    const u64 unlinks = c->vc.unlinks;
    pthread_mutex_unlock(&c->vc.mu);
    CASE_CHECK(c, renames >= 2); /* segment 1 at format and at least one rotation */
    CASE_CHECK(c, unlinks >= 1);
    check(c, FAULT_RANDOM_UNSYNCED, RC_NONE, 0, NULL);
    finish(c);
    case_free(c);
    return points;
}

/* ---- tests ---- */

static void test_crash_every_point(void)
{
    u32 cases = 0;
    u32 crashed = 0;
    u32 rc_crashed = 0;
    u32 enospc = 0;
    u64 discarded = 0;
    for (u32 cls = 0; cls < NCLASSES; cls++) {
        const u64 total = dry_points(cls, NULL);
        T_CHECK(total > 100);
        const u64 budget = 40u * (u64) t_scale();
        const u64 stride = total / budget > 0 ? total / budget : 1;
        for (u64 k = 1; k <= total; k += stride, cases++) {
            wcase *c = case_new(cls, k * 7919u + cls);
            fault_vfs_crash_at(c->fv, fault_vfs_points(c->fv) + k);
            workload(c, NOPS);
            crashed += fault_vfs_crashed(c->fv);
            const fault_mode mode = cases % 2 == 0 ? FAULT_DROP_UNSYNCED : FAULT_RANDOM_UNSYNCED;
            const rcrash rc = cases % 3 == 0 ? RC_POINT : RC_NONE;
            const u64 q = 1 + rng_below(&c->r2, 40);
            snprintf(
                c->desc,
                sizeof(c->desc),
                "class %u k %llu/%llu mode %d rc %d q %llu enospc %u",
                cls,
                (unsigned long long) k,
                (unsigned long long) total,
                (int) mode,
                (int) rc,
                (unsigned long long) q,
                c->enospc_injected);
            bool rcc = false;
            check(c, mode, rc, q, &rcc);
            rc_crashed += rcc;
            enospc += c->enospc_injected > 0;
            if (cases % 4 == 0) {
                finish(c);
            }
            discarded += c->discarded;
            case_free(c);
        }
    }
    T_CHECK(crashed * 10 >= cases * 9);
    T_CHECK(rc_crashed > 0);
    T_CHECK(enospc > 0);
    T_CHECK(discarded > 0);
}

static void test_double_crash(void)
{
    u64 discarded = 0;
    u32 second = 0;
    for (u64 seed = 1; seed <= 12u * (u64) t_scale(); seed++) {
        const u32 cls = (u32) (seed % NCLASSES);
        wcase *c = case_new(cls, seed * 104729u);
        const u64 k1 = 1 + rng_below(&c->r2, 600);
        const u64 k2 = 1 + rng_below(&c->r2, 600);
        snprintf(
            c->desc,
            sizeof(c->desc),
            "double class %u seed %llu k1 %llu k2 %llu",
            cls,
            (unsigned long long) seed,
            (unsigned long long) k1,
            (unsigned long long) k2);
        fault_vfs_crash_at(c->fv, fault_vfs_points(c->fv) + k1);
        workload(c, NOPS);
        const u64 x1 = check(c, FAULT_RANDOM_UNSYNCED, RC_NONE, 0, NULL);
        fault_vfs_crash_at(c->fv, fault_vfs_points(c->fv) + k2);
        workload(c, NOPS);
        second += fault_vfs_crashed(c->fv);
        const fault_mode mode = seed % 2 == 0 ? FAULT_DROP_UNSYNCED : FAULT_RANDOM_UNSYNCED;
        const u64 x2 = check(c, mode, RC_NONE, 0, NULL);
        CASE_CHECK(c, x2 >= x1);
        finish(c);
        discarded += c->discarded;
        case_free(c);
    }
    T_CHECK(second > 0);
    T_CHECK(discarded > 0);
}

static void test_crash_in_rotation_truncation_seal(void)
{
    u32 cases = 0;
    u32 crashed = 0;
    u32 open_crashed = 0;
    static const u32 classes[] = {0, 2};
    for (u32 ci = 0; ci < 2; ci++) {
        const u32 cls = classes[ci];
        u64 calls = 0;
        dry_points(cls, &calls);
        T_CHECK(calls > 40);
        const u64 budget = 30u * (u64) t_scale();
        const u64 stride = calls / budget > 0 ? calls / budget : 1;
        for (u64 j = 1; j <= calls; j += stride, cases++) {
            wcase *c = case_new(cls, j * 31u + cls);
            wrap_arm(&c->vc, j);
            workload(c, NOPS);
            crashed += fault_vfs_crashed(c->fv);
            const fault_mode mode = cases % 2 == 0 ? FAULT_RANDOM_UNSYNCED : FAULT_DROP_UNSYNCED;
            /* Target the mutations at WAL open, including seal and publish. */
            const rcrash rc = cases % 2 == 0 ? RC_OPEN_CALL : RC_NONE;
            const u64 q = 1 + (cases / 2) % 12;
            snprintf(
                c->desc,
                sizeof(c->desc),
                "rotation class %u call %llu/%llu mode %d rc %d q %llu",
                cls,
                (unsigned long long) j,
                (unsigned long long) calls,
                (int) mode,
                (int) rc,
                (unsigned long long) q);
            bool rcc = false;
            check(c, mode, rc, q, &rcc);
            open_crashed += rcc;
            if (cases % 3 == 0) {
                finish(c);
            }
            case_free(c);
        }
    }
    T_CHECK(crashed * 10 >= cases * 9);
    T_CHECK(open_crashed * 3 >= cases / 2);
}

static void test_repeated_reopen(void)
{
    for (u64 seed = 1; seed <= 4u * (u64) t_scale(); seed++) {
        const u32 cls = (u32) (seed % NCLASSES);
        wcase *c = case_new(cls, seed * 15485863u);
        const u64 k = seed % 2 == 0 ? 0 : 1 + rng_below(&c->r2, 600);
        snprintf(
            c->desc,
            sizeof(c->desc),
            "reopen class %u seed %llu k %llu",
            cls,
            (unsigned long long) seed,
            (unsigned long long) k);
        if (k != 0) {
            fault_vfs_crash_at(c->fv, fault_vfs_points(c->fv) + k);
        }
        workload(c, NOPS);
        const u64 x = check(c, FAULT_RANDOM_UNSYNCED, RC_NONE, 0, NULL);
        for (u32 rep = 0; rep < 3; rep++) {
            CASE_CHECK(c, check(c, FAULT_RANDOM_UNSYNCED, RC_NONE, 0, NULL) == x);
        }
        finish(c);
        case_free(c);
    }
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_crash_every_point);
    T_RUN(test_double_crash);
    T_RUN(test_crash_in_rotation_truncation_seal);
    T_RUN(test_repeated_reopen);
    return t_done();
}
