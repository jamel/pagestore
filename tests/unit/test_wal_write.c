#include "fault_vfs.h"
#include "format.h"
#include "mem_backend.h"
#include "test.h"

#include "pagestore/pagestore.h"
#include "pagestore/vfs.h"
#include "pagestore/wal.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define PS 4096u
#define HDR PS_PAGE_HEADER_SIZE
#define SEG ((u64) 64 * 1024)
#define MAXREC (16u * 1024u)
#define BIG 4096u /* 15 records fit per 64 KiB segment after headers */
#define PER_SEG 15u
#define WALDIR "/s/wal"

static fault_vfs *fv;
static const ps_vfs *vfs;

static ps_config store_config(void)
{
    ps_config cfg;
    ps_config_init(&cfg);
    cfg.page_size = PS;
    cfg.cache_size = (usize) 64 * PS;
    cfg.checkpoint_thread = false;
    cfg.vfs = fault_vfs_get(fv);
    return cfg;
}

static ps_wal_config wal_config(void)
{
    ps_wal_config c;
    ps_wal_config_init(&c);
    c.segment_size = SEG;
    c.max_record_size = MAXREC;
    return c;
}

static ps_store *setup(void)
{
    fv = fault_vfs_create(1);
    vfs = fault_vfs_get(fv);
    const ps_config cfg = store_config();
    ps_store *s = NULL;
    T_OK(ps_open("/s", &cfg, PS_CREATE, &s));
    return s;
}

static ps_wal *open_wal(ps_store *s)
{
    const ps_wal_config c = wal_config();
    ps_wal *w = NULL;
    T_OK(ps_wal_open(s, NULL, &c, &w));
    return w;
}

static void teardown(void)
{
    fault_vfs_destroy(fv);
    fv = NULL;
    vfs = NULL;
}

static void seg_name(char *buf, usize n, u64 seq)
{
    snprintf(buf, n, WALDIR "/%016llx.wal", (unsigned long long) seq);
}

static bool file_exists(const char *path)
{
    ps_file f;
    if (vfs->open(vfs->ctx, path, 0, &f) != 0) {
        return false;
    }
    vfs->close(vfs->ctx, &f);
    return true;
}

static bool seg_exists(u64 seq)
{
    char p[64];
    seg_name(p, sizeof(p), seq);
    return file_exists(p);
}

/* The caller owns the returned buffer, even for an empty file. */
static u8 *read_file(const char *path, u64 *size)
{
    ps_file f;
    T_OK(vfs->open(vfs->ctx, path, 0, &f));
    T_OK(vfs->size(vfs->ctx, &f, size));
    u8 *buf = t_malloc(*size);
    usize got = 0;
    T_OK(vfs->pread(vfs->ctx, &f, buf, *size, 0, &got));
    T_EQ(got, *size);
    vfs->close(vfs->ctx, &f);
    return buf;
}

static u8 *read_seg(u64 seq, u64 *size, ps_wal_seg_header *h)
{
    char p[64];
    seg_name(p, sizeof(p), seq);
    u8 *buf = read_file(p, size);
    T_CHECK(*size >= PS_WAL_SEG_HEADER_SIZE);
    T_OK(ps_wal_seg_header_decode(buf, h));
    T_EQ(h->seq, seq);
    return buf;
}

static u32 rec_len(u64 lsn)
{
    return (u32) (lsn * 37u % 300u); /* exercise empty payloads and padding */
}

static void fill_payload(u8 *p, u64 lsn, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        p[i] = (u8) (lsn * 7u + i * 13u);
    }
}

static void append_rec(ps_wal *w, u64 lsn, u32 len)
{
    u8 buf[MAXREC + 8];
    fill_payload(buf, lsn, len);
    T_OK(ps_wal_append(w, ps_lsn_from(lsn), buf, len));
}

/*
 * A fixed_len of 0 uses rec_len. Returns the offset after the last
 * expected record.
 */
static u64 check_seg(u64 seq, u64 prev, u64 first, u64 last, u32 fixed_len)
{
    u64 size = 0;
    ps_wal_seg_header h;
    u8 *buf = read_seg(seq, &size, &h);
    T_EQ(h.prev_lsn.raw, prev);
    u64 off = PS_WAL_SEG_HEADER_SIZE;
    u64 after = prev;
    for (u64 lsn = first; lsn <= last; lsn++) {
        ps_lsn got = PS_LSN_NONE;
        const u8 *payload = NULL;
        u32 len = 0;
        usize rsize = 0;
        T_OK(ps_wal_rec_decode(
            buf + off,
            size - off,
            ps_lsn_from(after),
            &got,
            &payload,
            &len,
            &rsize));
        T_EQ(got.raw, lsn);
        const u32 want = fixed_len != 0 ? fixed_len : rec_len(lsn);
        T_EQ(len, want);
        u8 exp[MAXREC + 8];
        fill_payload(exp, lsn, len);
        T_MEMEQ(payload, exp, len);
        off += rsize;
        after = lsn;
    }
    ps_lsn l = PS_LSN_NONE;
    const u8 *pl = NULL;
    u32 n = 0;
    usize rs = 0;
    if (off < size) {
        T_ERR(
            ps_wal_rec_decode(buf + off, size - off, ps_lsn_from(after), &l, &pl, &n, &rs),
            EBADMSG);
    }
    free(buf);
    return off;
}

/* ---- list helpers ---- */

struct ps_vfs_list_ctx {
    u32 tmps;
    u32 segs;
};

static int count_entry(ps_vfs_list_ctx *c, const char *name)
{
    const usize n = strlen(name);
    if (n > 4 && strcmp(name + n - 4, ".tmp") == 0) {
        c->tmps++;
    } else if (n > 4 && strcmp(name + n - 4, ".wal") == 0) {
        c->segs++;
    }
    return 0;
}

static ps_vfs_list_ctx count_files(void)
{
    ps_vfs_list_ctx c = {0, 0};
    T_OK(vfs->list(vfs->ctx, WALDIR, count_entry, &c));
    return c;
}

/* ---- tests ---- */

static void test_open_fresh(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    u64 size = 0;
    ps_wal_seg_header h;
    u8 *buf = read_seg(1, &size, &h);
    T_EQ(size, SEG);
    T_EQ(h.prev_lsn.raw, 0);
    for (u64 i = PS_WAL_SEG_HEADER_SIZE; i < size; i++) {
        T_EQ(buf[i], 0);
    }
    free(buf);
    T_CHECK(file_exists(WALDIR "/LOCK"));
    T_EQ(count_files().tmps, 0);

    const ps_wal_config c = wal_config();
    ps_wal *w2 = NULL;
    T_ERR(ps_wal_open(s, NULL, &c, &w2), EBUSY);

    ps_close(s);
    ps_wal_close(w);
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    T_CHECK(seg_exists(1));
    buf = read_seg(1, &size, &h);
    T_EQ(h.prev_lsn.raw, 0);
    free(buf);
    teardown();
}

static void test_open_fresh_nonzero_durable(void)
{
    ps_store *s = setup();
    T_OK(ps_op_begin(s, ps_lsn_from(5)));
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    T_EQ(ps_durable_lsn(s).raw, 5);
    fault_vfs_mkdir_durable(fv, WALDIR);
    const ps_wal_config c = wal_config();
    ps_wal *w = NULL;
    T_ERR(ps_wal_open(s, NULL, &c, &w), EBADMSG);
    T_EQ(count_files().segs, 0);
    T_ERR(ps_wal_open(s, NULL, &c, &w), EBADMSG);
    ps_close(s);
    teardown();
}

static void test_open_errors(void)
{
    ps_store *s = setup();
    ps_wal *w = NULL;
    ps_wal_config c = wal_config();
    c.segment_size = 64 * 1024 - 1;
    c.max_record_size = 1024;
    T_ERR(ps_wal_open(s, NULL, &c, &w), EINVAL);
    c = wal_config();
    c.max_record_size = (u32) (SEG - 56 + 1);
    T_ERR(ps_wal_open(s, NULL, &c, &w), EINVAL);
    /* Reserve 40 bytes for the segment header and 16 for the record. */
    c.max_record_size = (u32) (SEG - 56);
    T_OK(ps_wal_open(s, NULL, &c, &w));
    static u8 big[SEG - 56];
    memset(big, 0x5a, sizeof(big));
    T_OK(ps_wal_append(w, ps_lsn_from(1), big, sizeof(big)));
    T_OK(ps_wal_append(w, ps_lsn_from(2), big, sizeof(big)));
    T_OK(ps_wal_flush(w, ps_lsn_from(2)));
    T_CHECK(seg_exists(2));
    u64 size = 0;
    ps_wal_seg_header h;
    u8 *buf = read_seg(1, &size, &h);
    T_EQ(size, SEG);
    free(buf);

    ps_wal_config d;
    ps_wal_config_init(&d);
    T_EQ(d.segment_size, (u64) 64 << 20);
    T_EQ(d.buffer_size, (usize) 1 << 20);
    T_EQ(d.max_record_size, 16u << 20);
    ps_close(s);
    ps_wal_close(w);

    mem_backend *mb = mb_create();
    ps_backend be;
    mb_backend(mb, &be);
    ps_config cfg = store_config();
    cfg.vfs = NULL;
    T_OK(ps_open_backend(&be, &cfg, PS_CREATE, &s));
    c = wal_config();
    T_ERR(ps_wal_open(s, NULL, &c, &w), EINVAL);
    ps_close(s);
    mb_destroy(mb);
    teardown();
}

static void test_append_flush_durable(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    for (u64 lsn = 1; lsn <= 10; lsn++) {
        append_rec(w, lsn, rec_len(lsn));
    }
    T_OK(ps_wal_flush(w, ps_lsn_from(10)));
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    ps_close(s);
    ps_wal_close(w);
    check_seg(1, 0, 1, 10, 0);
    teardown();
}

static void test_unflushed_lost(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    for (u64 lsn = 1; lsn <= 4; lsn++) {
        append_rec(w, lsn, rec_len(lsn));
    }
    T_OK(ps_wal_flush(w, ps_lsn_from(2))); /* target is above the last appended LSN */
    append_rec(w, 5, 10);
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    ps_close(s);
    ps_wal_close(w);
    check_seg(1, 0, 1, 4, 0);
    teardown();
}

static void test_lsn_increasing(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    T_ERR(ps_wal_append(w, PS_LSN_NONE, NULL, 0), EINVAL);
    append_rec(w, 5, 8);
    T_ERR(ps_wal_append(w, ps_lsn_from(5), "x", 1), EINVAL);
    T_ERR(ps_wal_append(w, ps_lsn_from(4), "x", 1), EINVAL);
    append_rec(w, 7, 9);
    T_OK(ps_wal_flush(w, ps_lsn_from(7)));
    ps_close(s);
    ps_wal_close(w);
    u64 size = 0;
    ps_wal_seg_header h;
    u8 *buf = read_seg(1, &size, &h);
    ps_lsn lsn = PS_LSN_NONE;
    const u8 *p = NULL;
    u32 len = 0;
    usize rs = 0;
    u64 off = PS_WAL_SEG_HEADER_SIZE;
    T_OK(ps_wal_rec_decode(buf + off, size - off, PS_LSN_NONE, &lsn, &p, &len, &rs));
    T_EQ(lsn.raw, 5);
    off += rs;
    T_OK(ps_wal_rec_decode(buf + off, size - off, ps_lsn_from(5), &lsn, &p, &len, &rs));
    T_EQ(lsn.raw, 7);
    free(buf);
    teardown();
}

static void test_record_too_big(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    static u8 big[MAXREC + 1];
    T_ERR(ps_wal_append(w, ps_lsn_from(1), big, MAXREC + 1), EINVAL);
    append_rec(w, 1, MAXREC);
    T_OK(ps_wal_flush(w, ps_lsn_from(1)));
    ps_close(s);
    ps_wal_close(w);
    check_seg(1, 0, 1, 1, MAXREC);
    teardown();
}

/* Mix direct and buffered writes to check that both paths preserve record order. */
static void test_small_buffer(void)
{
    ps_store *s = setup();
    ps_wal_config c = wal_config();
    c.buffer_size = 1000;
    ps_wal *w = NULL;
    T_OK(ps_wal_open(s, NULL, &c, &w));
    u64 lsn = 1;
    for (; lsn <= 40; lsn++) {
        const u32 len = lsn % 5 == 0 ? 3001 + (u32) lsn : rec_len(lsn);
        append_rec(w, lsn, len);
    }
    T_OK(ps_wal_flush(w, PS_LSN_MAX));
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    ps_close(s);
    ps_wal_close(w);
    u64 size = 0;
    ps_wal_seg_header h;
    u8 *buf = read_seg(1, &size, &h);
    u64 off = PS_WAL_SEG_HEADER_SIZE;
    u64 after = 0;
    for (u64 i = 1; i <= 40; i++) {
        const u8 *p = NULL;
        u32 len = 0;
        usize rs = 0;
        ps_lsn got = PS_LSN_NONE;
        T_OK(ps_wal_rec_decode(buf + off, size - off, ps_lsn_from(after), &got, &p, &len, &rs));
        T_EQ(got.raw, i);
        T_EQ(len, i % 5 == 0 ? 3001 + (u32) i : rec_len(i));
        u8 exp[MAXREC];
        fill_payload(exp, i, len);
        T_MEMEQ(p, exp, len);
        off += rs;
        after = i;
    }
    free(buf);
    teardown();
}

static void test_rotation(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    u64 lsn = 0;
    while (!seg_exists(2)) {
        lsn++;
        append_rec(w, lsn, BIG);
        T_CHECK(lsn <= PER_SEG + 1);
    }
    T_EQ(lsn, PER_SEG + 1);
    u64 size = 0;
    ps_wal_seg_header h;
    free(read_seg(1, &size, &h));
    T_EQ(size, PS_WAL_SEG_HEADER_SIZE + PER_SEG * ps_wal_rec_size(BIG));
    T_EQ(check_seg(1, 0, 1, PER_SEG, BIG), size);
    free(read_seg(2, &size, &h));
    T_EQ(h.prev_lsn.raw, PER_SEG);
    T_EQ(size, SEG);
    T_EQ(count_files().tmps, 0);
    /* Rotation alone must make the old segment and new header durable. */
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    ps_close(s);
    ps_wal_close(w);
    T_CHECK(seg_exists(2));
    free(read_seg(1, &size, &h));
    T_EQ(check_seg(1, 0, 1, PER_SEG, BIG), size);
    T_EQ(size, PS_WAL_SEG_HEADER_SIZE + PER_SEG * ps_wal_rec_size(BIG));
    free(read_seg(2, &size, &h));
    T_EQ(h.prev_lsn.raw, PER_SEG);
    T_EQ(count_files().tmps, 0);
    teardown();
}

typedef struct flush_arg {
    ps_wal *w;
    u64 lsn;
    int rv;
} flush_arg;

static void *flush_thread(void *p)
{
    flush_arg *a = p;
    a->rv = ps_wal_flush(a->w, ps_lsn_from(a->lsn));
    return NULL;
}

static void test_concurrent_flush(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    for (u32 round = 0; round < 50; round++) {
        const u64 base = (u64) round * 10;
        for (u64 lsn = base + 1; lsn <= base + 10; lsn++) {
            append_rec(w, lsn, rec_len(lsn) % 64);
        }
        flush_arg a[2] = {{w, base + 10, -1}, {w, base + 10, -1}};
        pthread_t t[2];
        for (u32 i = 0; i < 2; i++) {
            T_OK(pthread_create(&t[i], NULL, flush_thread, &a[i]));
        }
        for (u32 i = 0; i < 2; i++) {
            T_OK(pthread_join(t[i], NULL));
            T_OK(a[i].rv);
        }
    }
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    ps_close(s);
    ps_wal_close(w);
    u64 size = 0;
    ps_wal_seg_header h;
    u8 *buf = read_seg(1, &size, &h);
    u64 off = PS_WAL_SEG_HEADER_SIZE;
    for (u64 lsn = 1; lsn <= 500; lsn++) {
        ps_lsn got = PS_LSN_NONE;
        const u8 *p = NULL;
        u32 len = 0;
        usize rs = 0;
        T_OK(ps_wal_rec_decode(buf + off, size - off, ps_lsn_from(lsn - 1), &got, &p, &len, &rs));
        T_EQ(got.raw, lsn);
        T_EQ(len, rec_len(lsn) % 64);
        off += rs;
    }
    free(buf);
    teardown();
}

/* Flushes on other threads while the appender keeps appending and rotating. */
typedef struct race_arg {
    ps_wal *w;
    _Atomic u64 *appended;
    _Atomic bool *stop;
} race_arg;

/* Read durable images so this check can run while the writer is rotating. */
static u64 durable_through(void)
{
    u8 *buf = t_malloc(SEG);
    u64 prev = 0;
    for (u64 seq = 1;; seq++) {
        char p[64];
        seg_name(p, sizeof(p), seq);
        usize got = 0;
        if (fault_vfs_read_durable(fv, p, 0, buf, SEG, &got) != 0 || got < PS_WAL_SEG_HEADER_SIZE) {
            break;
        }
        ps_wal_seg_header h;
        if (ps_wal_seg_header_decode(buf, &h) != 0 || h.prev_lsn.raw != prev) {
            break;
        }
        usize off = PS_WAL_SEG_HEADER_SIZE;
        for (;;) {
            ps_lsn lsn = PS_LSN_NONE;
            const u8 *pl = NULL;
            u32 len = 0;
            usize rs = 0;
            if (ps_wal_rec_decode(buf + off, got - off, ps_lsn_from(prev), &lsn, &pl, &len, &rs) !=
                0) {
                break;
            }
            T_EQ(lsn.raw, prev + 1);
            prev = lsn.raw;
            off += rs;
        }
    }
    free(buf);
    return prev;
}

static void *flush_loop(void *p)
{
    race_arg *a = p;
    while (!atomic_load(a->stop)) {
        const u64 l = atomic_load(a->appended);
        T_OK(ps_wal_flush(a->w, ps_lsn_from(l)));
        T_CHECK(durable_through() >= l);
    }
    return NULL;
}

static void test_flush_while_appending(void)
{
    ps_store *s = setup();
    ps_wal_config c = wal_config();
    c.buffer_size = 8192;
    ps_wal *w = NULL;
    T_OK(ps_wal_open(s, NULL, &c, &w));
    _Atomic u64 appended = 0;
    _Atomic bool stop = false;
    race_arg a = {w, &appended, &stop};
    pthread_t t[2];
    for (u32 i = 0; i < 2; i++) {
        T_OK(pthread_create(&t[i], NULL, flush_loop, &a));
    }
    const u64 n = 200;
    for (u64 lsn = 1; lsn <= n; lsn++) {
        append_rec(w, lsn, lsn % 7 == 0 ? 5000 : 900);
        atomic_store(&appended, lsn);
    }
    atomic_store(&stop, true);
    for (u32 i = 0; i < 2; i++) {
        T_OK(pthread_join(t[i], NULL));
    }
    T_OK(ps_wal_flush(w, ps_lsn_from(n)));
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    ps_close(s);
    ps_wal_close(w);
    u64 next = 1;
    u64 prev = 0;
    for (u64 seq = 1; seg_exists(seq); seq++) {
        u64 size = 0;
        ps_wal_seg_header h;
        u8 *buf = read_seg(seq, &size, &h);
        T_EQ(h.prev_lsn.raw, prev);
        u64 off = PS_WAL_SEG_HEADER_SIZE;
        for (;;) {
            ps_lsn got = PS_LSN_NONE;
            const u8 *p = NULL;
            u32 len = 0;
            usize rs = 0;
            if (ps_wal_rec_decode(buf + off, size - off, ps_lsn_from(prev), &got, &p, &len, &rs) !=
                0) {
                break;
            }
            T_EQ(got.raw, next);
            T_EQ(len, got.raw % 7 == 0 ? 5000 : 900);
            next++;
            prev = got.raw;
            off += rs;
        }
        free(buf);
    }
    T_EQ(next, n + 1);
    teardown();
}

static void test_flush_beyond_appended(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    T_OK(ps_wal_flush(w, ps_lsn_from(100)));
    for (u64 lsn = 1; lsn <= 3; lsn++) {
        append_rec(w, lsn, rec_len(lsn));
    }
    T_OK(ps_wal_flush(w, ps_lsn_from(100)));
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    ps_close(s);
    ps_wal_close(w);
    check_seg(1, 0, 1, 3, 0);
    teardown();
}

static void test_truncate(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    const u64 last = 3 * PER_SEG + 1;
    for (u64 lsn = 1; lsn <= last; lsn++) {
        append_rec(w, lsn, BIG);
    }
    for (u64 seq = 1; seq <= 4; seq++) {
        T_CHECK(seg_exists(seq));
    }
    T_OK(ps_wal_truncate(w, ps_lsn_from(2 * PER_SEG - 1)));
    T_CHECK(!seg_exists(1));
    T_CHECK(seg_exists(2));
    T_OK(ps_wal_truncate(w, ps_lsn_from(2 * PER_SEG)));
    T_CHECK(!seg_exists(2));
    T_CHECK(seg_exists(3));
    T_CHECK(seg_exists(4));
    T_OK(ps_wal_truncate(w, PS_LSN_MAX));
    T_CHECK(!seg_exists(3));
    T_CHECK(seg_exists(4));
    T_OK(ps_wal_truncate(w, PS_LSN_MAX));
    T_CHECK(seg_exists(4));
    append_rec(w, last + 1, BIG);
    T_OK(ps_wal_flush(w, ps_lsn_from(last + 1)));
    ps_close(s);
    ps_wal_close(w);
    check_seg(4, 3 * PER_SEG, last, last + 1, BIG);
    teardown();
}

static void test_enospc_rotation_healthy(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    for (u64 lsn = 1; lsn <= PER_SEG; lsn++) {
        append_rec(w, lsn, BIG);
    }
    fault_vfs_enospc_after(fv, 1);
    u8 buf[BIG];
    fill_payload(buf, PER_SEG + 1, BIG);
    u64 points = fault_vfs_points(fv);
    T_ERR(ps_wal_append(w, ps_lsn_from(PER_SEG + 1), buf, BIG), ENOSPC);
    /* The seal uses three points. The other three are temp create, preallocate and cleanup. */
    T_EQ(fault_vfs_points(fv) - points, 6);
    T_EQ(count_files().tmps, 0);
    T_CHECK(!seg_exists(2));
    /* The failed publish left a durable seal, so flush needs no I/O. */
    points = fault_vfs_points(fv);
    T_OK(ps_wal_flush(w, ps_lsn_from(PER_SEG)));
    T_EQ(fault_vfs_points(fv), points);
    /* Retry only the temp file operations. */
    T_ERR(ps_wal_append(w, ps_lsn_from(PER_SEG + 1), buf, BIG), ENOSPC);
    T_EQ(fault_vfs_points(fv) - points, 3);
    /* Even a small record needs a new segment after sealing. */
    T_ERR(ps_wal_append(w, ps_lsn_from(PER_SEG + 1), buf, 8), ENOSPC);
    T_EQ(fault_vfs_points(fv) - points, 6);
    fault_vfs_enospc_after(fv, UINT64_MAX);
    T_OK(ps_wal_append(w, ps_lsn_from(PER_SEG + 1), buf, BIG));
    T_CHECK(seg_exists(2));
    T_OK(ps_wal_flush(w, ps_lsn_from(PER_SEG + 1)));
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    ps_close(s);
    ps_wal_close(w);
    check_seg(1, 0, 1, PER_SEG, BIG);
    check_seg(2, PER_SEG, PER_SEG + 1, PER_SEG + 1, BIG);
    T_EQ(count_files().tmps, 0);
    teardown();
}

static void test_write_error_fails_wal(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    append_rec(w, 1, 100);
    /* Fail the fsync at point 2, after the buffer write. */
    fault_vfs_fail_at(fv, fault_vfs_points(fv) + 2, EIO);
    T_ERR(ps_wal_flush(w, ps_lsn_from(1)), EIO);
    fault_vfs_fail_at(fv, 0, 0);
    const u64 points = fault_vfs_points(fv);
    T_ERR(ps_wal_append(w, ps_lsn_from(2), "x", 1), EIO);
    T_ERR(ps_wal_flush(w, ps_lsn_from(1)), EIO);
    T_ERR(ps_wal_flush(w, PS_LSN_NONE), EIO);
    T_EQ(fault_vfs_points(fv), points);

    T_OK(ps_op_begin(s, ps_lsn_from(1)));
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    ps_op_commit(s);
    T_ERR(ps_checkpoint(s), EIO);
    struct ps_stats st;
    ps_stats(s, &st);
    T_CHECK(st.failed);
    T_EQ(ps_durable_lsn(s).raw, 0);
    ps_close(s);
    ps_wal_close(w);
    teardown();
}

/* A failed write while draining a full buffer, and a failed seal, fail the WAL too. */
static void test_drain_and_seal_errors_fail_wal(void)
{
    ps_store *s = setup();
    ps_wal_config c = wal_config();
    c.buffer_size = 1024;
    ps_wal *w = NULL;
    T_OK(ps_wal_open(s, NULL, &c, &w));
    append_rec(w, 1, 1000);
    T_OK(ps_wal_flush(w, ps_lsn_from(1)));
    append_rec(w, 2, 1000); /* 1016 of 1024 bytes buffered forces the next append to drain */
    fault_vfs_fail_at(fv, fault_vfs_points(fv) + 1, ENOSPC);
    T_ERR(
        ps_wal_append(w, ps_lsn_from(3), "abc", 3),
        EIO); /* disk exhaustion during a write is fatal */
    T_ERR(ps_wal_append(w, ps_lsn_from(4), "abc", 3), EIO);
    T_ERR(ps_wal_flush(w, ps_lsn_from(4)), EIO);
    check_seg(1, 0, 1, 1, 1000);
    fault_vfs_reboot(fv, FAULT_RANDOM_UNSYNCED);
    ps_close(s);
    ps_wal_close(w);
    check_seg(1, 0, 1, 1, 1000);
    teardown();

    s = setup();
    w = open_wal(s);
    for (u64 lsn = 1; lsn <= PER_SEG; lsn++) {
        append_rec(w, lsn, BIG);
    }
    /* Fail point 3 to hit the seal fsync after drain and truncate. */
    fault_vfs_fail_at(fv, fault_vfs_points(fv) + 3, EIO);
    u8 buf[BIG];
    fill_payload(buf, PER_SEG + 1, BIG);
    T_ERR(ps_wal_append(w, ps_lsn_from(PER_SEG + 1), buf, BIG), EIO);
    T_ERR(ps_wal_flush(w, ps_lsn_from(PER_SEG)), EIO);
    T_CHECK(!seg_exists(2));
    ps_close(s);
    ps_wal_close(w);
    teardown();
}

/* Fill segment 1 so the next BIG append exercises rotation. */
static void fill_seg1(ps_wal *w)
{
    for (u64 lsn = 1; lsn <= PER_SEG; lsn++) {
        append_rec(w, lsn, BIG);
    }
}

static void check_failed(ps_wal *w)
{
    const u64 points = fault_vfs_points(fv);
    u8 buf[BIG];
    fill_payload(buf, PER_SEG + 1, BIG);
    T_ERR(ps_wal_append(w, ps_lsn_from(PER_SEG + 1), buf, BIG), EIO);
    T_ERR(ps_wal_append(w, ps_lsn_from(PER_SEG + 1), buf, 8), EIO);
    T_ERR(ps_wal_flush(w, ps_lsn_from(PER_SEG)), EIO);
    T_EQ(fault_vfs_points(fv), points);
}

/*
 * Points 1 through 3 drain, truncate and sync the old segment. The next
 * two create and preallocate the temp file. Points 6 through 9 write and
 * sync the header, rename the file and sync the directory.
 */
static void test_publish_errors_fatal(void)
{
    for (u64 k = 6; k <= 9; k++) {
        ps_store *s = setup();
        ps_wal *w = open_wal(s);
        fill_seg1(w);
        const u64 base = fault_vfs_points(fv);
        fault_vfs_fail_at(fv, base + k, EIO);
        u8 buf[BIG];
        fill_payload(buf, PER_SEG + 1, BIG);
        T_ERR(ps_wal_append(w, ps_lsn_from(PER_SEG + 1), buf, BIG), EIO);
        T_EQ(fault_vfs_points(fv), base + k);
        fault_vfs_fail_at(fv, 0, 0);
        check_failed(w);
        T_EQ(seg_exists(2), k == 9); /* rename is visible if directory sync fails */
        fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
        ps_close(s);
        ps_wal_close(w);
        T_CHECK(!seg_exists(2));
        check_seg(1, 0, 1, PER_SEG, BIG);
        teardown();
    }
}

/* A leftover temp file blocks the next exclusive create. */
static void test_enospc_cleanup_failure_fatal(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    fill_seg1(w);
    fault_vfs_enospc_after(fv, 1);
    /* Fail point 6, the cleanup after preallocate returns ENOSPC. */
    fault_vfs_fail_at(fv, fault_vfs_points(fv) + 6, EIO);
    u8 buf[BIG];
    fill_payload(buf, PER_SEG + 1, BIG);
    T_ERR(ps_wal_append(w, ps_lsn_from(PER_SEG + 1), buf, BIG), EIO);
    fault_vfs_fail_at(fv, 0, 0);
    fault_vfs_enospc_after(fv, UINT64_MAX);
    check_failed(w);
    ps_close(s);
    ps_wal_close(w);
    teardown();
}

/* A space error creating the temporary file leaves rotation retryable. */
static void test_create_disk_full_clean(void)
{
    const int errs[2] = {ENOSPC, EDQUOT};
    for (u32 i = 0; i < 2; i++) {
        ps_store *s = setup();
        ps_wal *w = open_wal(s);
        fill_seg1(w);
        fault_vfs_fail_at(fv, fault_vfs_points(fv) + 4, errs[i]);
        u8 buf[BIG];
        fill_payload(buf, PER_SEG + 1, BIG);
        T_ERR(ps_wal_append(w, ps_lsn_from(PER_SEG + 1), buf, BIG), errs[i]);
        T_EQ(count_files().tmps, 0);
        T_CHECK(!seg_exists(2));
        T_OK(ps_wal_flush(w, ps_lsn_from(PER_SEG)));
        T_OK(ps_wal_append(w, ps_lsn_from(PER_SEG + 1), buf, BIG));
        T_OK(ps_wal_flush(w, ps_lsn_from(PER_SEG + 1)));
        fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
        ps_close(s);
        ps_wal_close(w);
        check_seg(1, 0, 1, PER_SEG, BIG);
        check_seg(2, PER_SEG, PER_SEG + 1, PER_SEG + 1, BIG);
        teardown();
    }

    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    fill_seg1(w);
    fault_vfs_fail_at(fv, fault_vfs_points(fv) + 5, EDQUOT);
    u8 buf[BIG];
    fill_payload(buf, PER_SEG + 1, BIG);
    T_ERR(ps_wal_append(w, ps_lsn_from(PER_SEG + 1), buf, BIG), EDQUOT);
    T_EQ(count_files().tmps, 0);
    T_OK(ps_wal_append(w, ps_lsn_from(PER_SEG + 1), buf, BIG));
    T_OK(ps_wal_flush(w, ps_lsn_from(PER_SEG + 1)));
    ps_close(s);
    ps_wal_close(w);
    teardown();
}

/* Retry after a failed unlink to check that truncation resumes. */
static void test_truncate_unlink_error(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    const u64 last = 2 * PER_SEG + 1;
    for (u64 lsn = 1; lsn <= last; lsn++) {
        append_rec(w, lsn, BIG);
    }
    fault_vfs_fail_at(fv, fault_vfs_points(fv) + 1, EIO);
    T_ERR(ps_wal_truncate(w, PS_LSN_MAX), EIO);
    fault_vfs_fail_at(fv, 0, 0);
    T_CHECK(seg_exists(1));
    T_CHECK(seg_exists(2));
    append_rec(w, last + 1, 100);
    T_OK(ps_wal_flush(w, ps_lsn_from(last + 1)));
    T_OK(ps_wal_truncate(w, PS_LSN_MAX));
    T_CHECK(!seg_exists(1));
    T_CHECK(!seg_exists(2));
    T_CHECK(seg_exists(3));
    ps_close(s);
    ps_wal_close(w);
    teardown();
}

/* After a flush on another thread failed the WAL, an append that must rotate writes nothing. */
static void *failing_flush(void *p)
{
    flush_arg *a = p;
    a->rv = ps_wal_flush(a->w, ps_lsn_from(a->lsn));
    return NULL;
}

static void test_no_io_after_failure(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    fill_seg1(w);
    /* Fail the other thread's fsync after the buffered bytes reach the file. */
    fault_vfs_fail_at(fv, fault_vfs_points(fv) + 2, EIO);
    flush_arg a = {w, PER_SEG, -1};
    pthread_t t;
    T_OK(pthread_create(&t, NULL, failing_flush, &a));
    T_OK(pthread_join(t, NULL));
    T_ERR(a.rv, EIO);
    fault_vfs_fail_at(fv, 0, 0);
    check_failed(w);
    T_CHECK(!seg_exists(2));
    ps_close(s);
    ps_wal_close(w);
    teardown();
}

static void test_hooks_installed(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    const u64 n = PER_SEG + 5;
    for (u64 lsn = 1; lsn <= n; lsn++) {
        T_OK(ps_op_begin(s, ps_lsn_from(lsn)));
        ps_page_id id = PS_PAGE_ID_NONE;
        ps_page *p = NULL;
        T_OK(ps_alloc(s, &id, &p));
        memset((u8 *) p + HDR, (int) lsn, 16);
        append_rec(w, lsn, BIG);
        ps_op_commit(s);
    }
    T_CHECK(seg_exists(1));
    T_CHECK(seg_exists(2));
    T_OK(ps_checkpoint(s));
    T_EQ(ps_durable_lsn(s).raw, n);
    T_CHECK(!seg_exists(1));
    T_CHECK(seg_exists(2));
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    ps_close(s);
    ps_wal_close(w);
    check_seg(2, PER_SEG, PER_SEG + 1, n, BIG); /* checkpoint was the only flush */
    teardown();
}

static void test_posix_smoke(void)
{
    char *root = t_tmpdir();
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s/store", root);
    ps_config cfg;
    ps_config_init(&cfg);
    cfg.cache_size = (usize) 64 * PS;
    cfg.checkpoint_thread = false;
    ps_store *s = NULL;
    T_OK(ps_open(dir, &cfg, PS_CREATE, &s));
    const ps_wal_config c = wal_config();
    ps_wal *w = NULL;
    T_OK(ps_wal_open(s, NULL, &c, &w));
    for (u64 lsn = 1; lsn <= 20; lsn++) {
        u8 buf[BIG];
        fill_payload(buf, lsn, BIG);
        T_OK(ps_wal_append(w, ps_lsn_from(lsn), buf, BIG));
    }
    T_OK(ps_wal_flush(w, ps_lsn_from(20)));
    ps_close(s);
    ps_wal_close(w);
    char path[4200];
    struct stat st;
    snprintf(path, sizeof(path), "%s/wal/0000000000000001.wal", dir);
    T_EQ(stat(path, &st), 0);
    T_EQ(st.st_size, PS_WAL_SEG_HEADER_SIZE + PER_SEG * ps_wal_rec_size(BIG));
    snprintf(path, sizeof(path), "%s/wal/0000000000000002.wal", dir);
    T_EQ(stat(path, &st), 0);
    T_EQ(st.st_size, SEG);
    snprintf(path, sizeof(path), "%s/wal/LOCK", dir);
    T_EQ(stat(path, &st), 0);
    t_rmtree(root);
    free(root);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_open_fresh);
    T_RUN(test_open_fresh_nonzero_durable);
    T_RUN(test_open_errors);
    T_RUN(test_append_flush_durable);
    T_RUN(test_unflushed_lost);
    T_RUN(test_lsn_increasing);
    T_RUN(test_record_too_big);
    T_RUN(test_small_buffer);
    T_RUN(test_rotation);
    T_RUN(test_concurrent_flush);
    T_RUN(test_flush_while_appending);
    T_RUN(test_flush_beyond_appended);
    T_RUN(test_truncate);
    T_RUN(test_enospc_rotation_healthy);
    T_RUN(test_write_error_fails_wal);
    T_RUN(test_drain_and_seal_errors_fail_wal);
    T_RUN(test_publish_errors_fatal);
    T_RUN(test_enospc_cleanup_failure_fatal);
    T_RUN(test_create_disk_full_clean);
    T_RUN(test_truncate_unlink_error);
    T_RUN(test_no_io_after_failure);
    T_RUN(test_hooks_installed);
    T_RUN(test_posix_smoke);
    return t_done();
}
