#include "fault_vfs.h"
#include "format.h"
#include "pagestore/pagestore.h"
#include "pagestore/wal.h"
#include "test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static ps_store *open_store(u32 flags)
{
    const ps_config cfg = store_config();
    ps_store *s = NULL;
    T_OK(ps_open("/s", &cfg, flags, &s));
    return s;
}

static ps_store *setup_seed(u64 seed)
{
    fv = fault_vfs_create(seed);
    vfs = fault_vfs_get(fv);
    return open_store(PS_CREATE);
}

static ps_store *setup(void)
{
    return setup_seed(1);
}

static int try_open_wal_cfg(ps_store *s, const ps_wal_config *c, ps_wal **w)
{
    *w = NULL;
    return ps_wal_open(s, NULL, c, w);
}

static ps_wal *open_wal_cfg(ps_store *s, const ps_wal_config *c)
{
    ps_wal *w = NULL;
    T_OK(try_open_wal_cfg(s, c, &w));
    return w;
}

static ps_wal *open_wal(ps_store *s)
{
    const ps_wal_config c = wal_config();
    return open_wal_cfg(s, &c);
}

static int try_open_wal(ps_store *s, ps_wal **w)
{
    const ps_wal_config c = wal_config();
    return try_open_wal_cfg(s, &c, w);
}

static void close_both(ps_store *s, ps_wal *w)
{
    ps_close(s);
    ps_wal_close(w);
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

static void unlink_seg(u64 seq)
{
    char p[64];
    seg_name(p, sizeof(p), seq);
    T_OK(vfs->unlink(vfs->ctx, p));
    T_OK(vfs->fsync_dir(vfs->ctx, WALDIR));
}

static u8 *read_file(const char *path, u64 *size)
{
    ps_file f;
    T_OK(vfs->open(vfs->ctx, path, 0, &f));
    T_OK(vfs->size(vfs->ctx, &f, size));
    u8 *buf = malloc(*size > 0 ? *size : 1);
    T_CHECK(buf != NULL);
    usize got = 0;
    T_OK(vfs->pread(vfs->ctx, &f, buf, *size, 0, &got));
    T_EQ(got, *size);
    vfs->close(vfs->ctx, &f);
    return buf;
}

static u64 seg_size(u64 seq)
{
    char p[64];
    seg_name(p, sizeof(p), seq);
    ps_file f;
    T_OK(vfs->open(vfs->ctx, p, 0, &f));
    u64 size = 0;
    T_OK(vfs->size(vfs->ctx, &f, &size));
    vfs->close(vfs->ctx, &f);
    return size;
}

static ps_wal_seg_header seg_header(u64 seq)
{
    char p[64];
    seg_name(p, sizeof(p), seq);
    u64 size = 0;
    u8 *buf = read_file(p, &size);
    T_CHECK(size >= PS_WAL_SEG_HEADER_SIZE);
    ps_wal_seg_header h;
    T_OK(ps_wal_seg_header_decode(buf, &h));
    T_EQ(h.seq, seq);
    free(buf);
    return h;
}

/* Number of *.wal files in the WAL directory, and of *.tmp files. */
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

static u32 rec_len(u64 lsn)
{
    return (u32) (lsn * 37u % 300u); /* exercise empty payloads and padding */
}

/* A different tag exposes stale payloads when the same LSNs are reused. */
static void fill_payload(u8 *p, u64 lsn, u32 len, u32 tag)
{
    for (u32 i = 0; i < len; i++) {
        p[i] = (u8) (lsn * 7u + i * 13u + tag * 101u);
    }
}

static int append_tagged(ps_wal *w, u64 lsn, u32 len, u32 tag)
{
    u8 buf[MAXREC + 8];
    fill_payload(buf, lsn, len, tag);
    return ps_wal_append(w, lsn, buf, len);
}

static void append_rec(ps_wal *w, u64 lsn, u32 len)
{
    T_OK(append_tagged(w, lsn, len, 0));
}

static void append_range(ps_wal *w, u64 first, u64 last, u32 fixed_len)
{
    for (u64 lsn = first; lsn <= last; lsn++) {
        append_rec(w, lsn, fixed_len != 0 ? fixed_len : rec_len(lsn));
    }
}

/*
 * fixed_len of 0 uses rec_len. tag_above applies only above tag_split.
 * An empty range returns from_lsn - 1, or 0 if from_lsn is 0.
 */
static u64 check_iter_tagged(
    ps_wal *w,
    u64 from_lsn,
    u32 fixed_len,
    u64 tag_split,
    u32 tag_above,
    u64 *count)
{
    ps_wal_iter *it = NULL;
    T_OK(ps_wal_iter_open(w, from_lsn, &it));
    u64 want = from_lsn == 0 ? 1 : from_lsn;
    u64 n = 0;
    for (;;) {
        u64 lsn = 0;
        const void *buf = NULL;
        usize len = 0;
        const int rv = ps_wal_iter_next(it, &lsn, &buf, &len);
        if (rv == ENOENT) {
            break;
        }
        T_OK(rv);
        T_EQ(lsn, want);
        const u32 l = fixed_len != 0 ? fixed_len : rec_len(lsn);
        T_EQ(len, l);
        u8 exp[MAXREC + 8];
        fill_payload(exp, lsn, l, lsn > tag_split ? tag_above : 0);
        T_MEMEQ(buf, exp, l);
        want++;
        n++;
    }
    u64 lsn = 0;
    const void *buf = NULL;
    usize len = 0;
    T_ERR(ps_wal_iter_next(it, &lsn, &buf, &len), ENOENT);
    ps_wal_iter_close(it);
    if (count != NULL) {
        *count = n;
    }
    return want - 1;
}

static u64 check_iter(ps_wal *w, u64 from_lsn, u32 fixed_len, u64 *count)
{
    return check_iter_tagged(w, from_lsn, fixed_len, UINT64_MAX, 0, count);
}

/* Advance the store LSN independently of the WAL by committing one allocation. */
static void commit_op(ps_store *s, u64 lsn)
{
    T_OK(ps_op_begin(s, lsn));
    u32 id = 0;
    ps_page *p = NULL;
    T_OK(ps_alloc(s, &id, &p));
    memset((u8 *) p + HDR, (int) lsn, 16);
    ps_op_commit(s);
}

/* Disable the hooks so we can keep old segments to test open. */
static void checkpoint_at(ps_store *s, u64 lsn)
{
    commit_op(s, lsn);
    ps_set_log_hooks(s, NULL);
    T_OK(ps_checkpoint(s));
    T_EQ(ps_durable_lsn(s), lsn);
}

/* ---- tests ---- */

static void test_reopen_lists_records(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    append_range(w, 1, 20, 0);
    T_OK(ps_wal_flush(w, 20));
    const u64 end1 = seg_size(1);
    T_EQ(end1, SEG);
    close_both(s, w);

    s = open_store(0);
    w = open_wal(s);
    u64 n = 0;
    T_EQ(check_iter(w, 1, 0, &n), 20);
    T_EQ(n, 20);
    T_EQ(check_iter(w, 0, 0, NULL), 20);
    u64 end = PS_WAL_SEG_HEADER_SIZE;
    for (u64 lsn = 1; lsn <= 20; lsn++) {
        end += ps_wal_rec_size(rec_len(lsn));
    }
    T_EQ(seg_size(1), end);
    T_EQ(seg_header(2).prev_lsn, 20);
    T_EQ(seg_size(2), SEG);
    T_EQ(count_files().segs, 2);
    T_EQ(count_files().tmps, 0);

    T_ERR(ps_wal_append(w, 20, NULL, 0), EINVAL);
    append_range(w, 21, 25, 0);
    T_OK(ps_wal_flush(w, 25));
    close_both(s, w);
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    s = open_store(0);
    w = open_wal(s);
    T_EQ(check_iter(w, 1, 0, NULL), 25);
    T_EQ(seg_header(3).prev_lsn, 25);
    close_both(s, w);
    teardown();
}

static void test_iter_spans_segments(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    const u64 n = 3 * PER_SEG + 4;
    append_range(w, 1, n, BIG);
    T_OK(ps_wal_flush(w, n));
    close_both(s, w);
    s = open_store(0);
    w = open_wal(s);
    T_CHECK(seg_exists(5));
    u64 cnt = 0;
    T_EQ(check_iter(w, 1, BIG, &cnt), n);
    T_EQ(cnt, n);
    T_EQ(check_iter(w, 7, BIG, &cnt), n);
    T_EQ(cnt, n - 6);
    T_EQ(check_iter(w, PER_SEG + 1, BIG, &cnt), n);
    T_EQ(cnt, n - PER_SEG);
    T_EQ(check_iter(w, 2 * PER_SEG, BIG, &cnt), n);
    T_EQ(cnt, n - 2 * PER_SEG + 1);
    T_EQ(check_iter(w, n, BIG, &cnt), n);
    T_EQ(cnt, 1);
    check_iter(w, n + 1, BIG, &cnt);
    T_EQ(cnt, 0);
    check_iter(w, UINT64_MAX, BIG, &cnt);
    T_EQ(cnt, 0);
    ps_wal_iter *a = NULL;
    ps_wal_iter *b = NULL;
    T_OK(ps_wal_iter_open(w, 1, &a));
    T_OK(ps_wal_iter_open(w, 20, &b));
    u64 la = 0;
    u64 lb = 0;
    const void *pa = NULL;
    const void *pb = NULL;
    usize na = 0;
    usize nb = 0;
    T_OK(ps_wal_iter_next(a, &la, &pa, &na));
    T_OK(ps_wal_iter_next(b, &lb, &pb, &nb));
    T_EQ(la, 1);
    T_EQ(lb, 20);
    ps_wal_iter_close(a);
    ps_wal_iter_close(b);
    ps_wal_iter_close(NULL);
    close_both(s, w);
    teardown();
}

static void test_iter_fresh_wal_empty(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    u64 cnt = 1;
    check_iter(w, 1, 0, &cnt);
    T_EQ(cnt, 0);
    close_both(s, w);
    teardown();
}

static void test_iter_invalid_after_append(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    append_range(w, 1, 5, 0);
    T_OK(ps_wal_flush(w, 5));
    close_both(s, w);
    s = open_store(0);
    w = open_wal(s);
    ps_wal_iter *it = NULL;
    T_OK(ps_wal_iter_open(w, 1, &it));
    u64 lsn = 0;
    const void *buf = NULL;
    usize len = 0;
    T_OK(ps_wal_iter_next(it, &lsn, &buf, &len));
    T_EQ(lsn, 1);
    append_rec(w, 6, 10);
    T_ERR(ps_wal_iter_next(it, &lsn, &buf, &len), EINVAL);
    ps_wal_iter_close(it);
    it = NULL;
    T_ERR(ps_wal_iter_open(w, 1, &it), EINVAL);
    T_CHECK(it == NULL);
    T_ERR(ps_wal_iter_open(NULL, 1, &it), EINVAL);
    close_both(s, w);
    teardown();
}

static void test_iter_holds_off_truncate(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    append_range(w, 1, 2 * PER_SEG + 3, BIG);
    T_OK(ps_wal_flush(w, 2 * PER_SEG + 3));
    close_both(s, w);
    s = open_store(0);
    w = open_wal(s);
    ps_wal_iter *it = NULL;
    T_OK(ps_wal_iter_open(w, 1, &it));
    /* Simulate truncation by a checkpoint while recovery has an iterator open. */
    T_OK(ps_wal_truncate(w, UINT64_MAX));
    T_CHECK(seg_exists(1));
    T_CHECK(seg_exists(2));
    u64 lsn = 0;
    const void *buf = NULL;
    usize len = 0;
    u64 want = 1;
    int rv = 0;
    while ((rv = ps_wal_iter_next(it, &lsn, &buf, &len)) == 0) {
        T_EQ(lsn, want);
        want++;
    }
    T_ERR(rv, ENOENT);
    T_EQ(want, 2 * PER_SEG + 4);
    ps_wal_iter_close(it);
    T_OK(ps_wal_truncate(w, UINT64_MAX));
    T_CHECK(!seg_exists(1));
    T_CHECK(!seg_exists(3));
    T_CHECK(seg_exists(4));
    close_both(s, w);
    teardown();
}

static void test_torn_tail(void)
{
    for (u64 seed = 1; seed <= 50; seed++) {
        ps_store *s = setup_seed(seed);
        ps_wal_config c = wal_config();
        c.buffer_size = 0; /* leave unsynced bytes in the file for the simulated crash */
        ps_wal *w = open_wal_cfg(s, &c);
        append_range(w, 1, 10, 0);
        T_OK(ps_wal_flush(w, 10));
        append_range(w, 11, 15, 0);
        fault_vfs_crash_at(fv, fault_vfs_points(fv) + 1);
        T_ERR(ps_wal_flush(w, 15), EIO);
        T_CHECK(fault_vfs_crashed(fv));
        fault_vfs_reboot(fv, FAULT_RANDOM_UNSYNCED);
        close_both(s, w);

        s = open_store(0);
        w = open_wal(s);
        const u64 k = check_iter(w, 1, 0, NULL);
        T_CHECK(k >= 10 && k <= 15);
        T_EQ(seg_header(2).prev_lsn, k);
        close_both(s, w);
        fault_vfs_reboot(fv, FAULT_RANDOM_UNSYNCED);
        s = open_store(0);
        w = open_wal(s);
        T_EQ(check_iter(w, 1, 0, NULL), k);
        close_both(s, w);
        teardown();
    }
}

static void build_segments(u64 last_lsn)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    append_range(w, 1, last_lsn, BIG);
    T_OK(ps_wal_flush(w, last_lsn));
    close_both(s, w);
}

static void test_corrupt_sealed_segment(void)
{
    const u64 offs[] = {
        PS_WAL_SEG_HEADER_SIZE + 20,
        PS_WAL_SEG_HEADER_SIZE + ps_wal_rec_size(BIG) * 3 + 4,
        PS_WAL_SEG_HEADER_SIZE + ps_wal_rec_size(BIG) * PER_SEG - 1,
    };
    for (u32 seg = 1; seg <= 2; seg++) {
        for (u32 i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
            build_segments(2 * PER_SEG + 3);
            char p[64];
            seg_name(p, sizeof(p), seg);
            T_OK(fault_vfs_corrupt(fv, p, offs[i], 0x40));
            ps_store *s = open_store(0);
            ps_wal *w = NULL;
            T_ERR(try_open_wal(s, &w), EBADMSG);
            T_EQ(count_files().segs, 3);
            ps_close(s);
            teardown();
        }
    }
    build_segments(2 * PER_SEG + 3);
    char p[64];
    seg_name(p, sizeof(p), 1);
    ps_file f;
    T_OK(vfs->open(vfs->ctx, p, 0, &f));
    T_OK(vfs->truncate(vfs->ctx, &f, seg_size(1) + 8));
    T_OK(vfs->fsync(vfs->ctx, &f));
    vfs->close(vfs->ctx, &f);
    ps_store *s = open_store(0);
    ps_wal *w = NULL;
    T_ERR(try_open_wal(s, &w), EBADMSG);
    ps_close(s);
    teardown();
    build_segments(2 * PER_SEG + 3);
    seg_name(p, sizeof(p), 2);
    T_OK(fault_vfs_corrupt(fv, p, 25, 0x01));
    s = open_store(0);
    T_ERR(try_open_wal(s, &w), EBADMSG);
    ps_close(s);
    teardown();
}

static void test_corrupt_newest_is_tail(void)
{
    /* Open treats the first damaged record in the newest segment as the end. */
    build_segments(PER_SEG + 6);
    char p[64];
    seg_name(p, sizeof(p), 2);
    T_OK(fault_vfs_corrupt(fv, p, PS_WAL_SEG_HEADER_SIZE + ps_wal_rec_size(BIG) * 2 + 30, 0x01));
    ps_store *s = open_store(0);
    ps_wal *w = open_wal(s);
    T_EQ(check_iter(w, 1, BIG, NULL), PER_SEG + 2);
    T_EQ(seg_header(3).prev_lsn, PER_SEG + 2);
    close_both(s, w);
    teardown();
}

static void test_broken_chain(void)
{
    /* The two segments are individually valid but disagree at the boundary. */
    build_segments(PER_SEG + 3);
    char p[64];
    seg_name(p, sizeof(p), 2);
    u64 size = 0;
    u8 *buf = read_file(p, &size);
    ps_wal_seg_header h;
    T_OK(ps_wal_seg_header_decode(buf, &h));
    h.prev_lsn = PER_SEG + 1;
    ps_wal_seg_header_encode(&h, buf);
    free(buf);
    ps_file f;
    T_OK(vfs->open(vfs->ctx, p, 0, &f));
    u8 hdr[PS_WAL_SEG_HEADER_SIZE];
    ps_wal_seg_header_encode(&h, hdr);
    T_OK(vfs->pwrite(vfs->ctx, &f, hdr, sizeof(hdr), 0));
    T_OK(vfs->fsync(vfs->ctx, &f));
    vfs->close(vfs->ctx, &f);
    ps_store *s = open_store(0);
    ps_wal *w = NULL;
    T_ERR(try_open_wal(s, &w), EBADMSG);
    ps_close(s);
    teardown();
}

static void test_missing_middle_segment(void)
{
    build_segments(2 * PER_SEG + 3);
    unlink_seg(2);
    ps_store *s = open_store(0);
    ps_wal *w = NULL;
    T_ERR(try_open_wal(s, &w), EBADMSG);
    T_CHECK(seg_exists(1));
    T_CHECK(seg_exists(3));
    ps_close(s);
    teardown();
}

static void test_obsolete_gap_tolerated(void)
{
    /* Checkpoint into segment 3 so a gap in older segments is safe. */
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    append_range(w, 1, 2 * PER_SEG + 5, BIG);
    T_OK(ps_wal_flush(w, 2 * PER_SEG + 5));
    checkpoint_at(s, 2 * PER_SEG + 2);
    close_both(s, w);
    T_CHECK(seg_exists(1));
    unlink_seg(2);
    s = open_store(0);
    T_EQ(ps_durable_lsn(s), 2 * PER_SEG + 2);
    w = open_wal(s);
    T_CHECK(!seg_exists(1));
    T_CHECK(seg_exists(3));
    T_CHECK(seg_exists(4));
    u64 cnt = 0;
    T_EQ(check_iter(w, 2 * PER_SEG + 1, BIG, &cnt), 2 * PER_SEG + 5);
    T_EQ(cnt, 5);
    ps_wal_iter *it = NULL;
    T_OK(ps_wal_iter_open(w, 1, &it));
    u64 lsn = 0;
    const void *buf = NULL;
    usize len = 0;
    T_OK(ps_wal_iter_next(it, &lsn, &buf, &len));
    T_EQ(lsn, 2 * PER_SEG + 1);
    ps_wal_iter_close(it);
    close_both(s, w);
    teardown();
}

static void test_obsolete_corrupt_ignored(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    append_range(w, 1, 2 * PER_SEG + 5, BIG);
    T_OK(ps_wal_flush(w, 2 * PER_SEG + 5));
    checkpoint_at(s, 2 * PER_SEG + 2);
    close_both(s, w);
    char p[64];
    seg_name(p, sizeof(p), 1);
    T_OK(fault_vfs_corrupt(fv, p, 0, 0xff));
    seg_name(p, sizeof(p), 2);
    T_OK(fault_vfs_corrupt(fv, p, PS_WAL_SEG_HEADER_SIZE + 100, 0xff));
    s = open_store(0);
    w = open_wal(s);
    T_EQ(count_files().segs, 2);
    T_EQ(check_iter(w, 2 * PER_SEG + 3, BIG, NULL), 2 * PER_SEG + 5);
    close_both(s, w);
    teardown();
}

static void test_anchor_is_newest(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    append_range(w, 1, 2 * PER_SEG + 5, BIG);
    T_OK(ps_wal_flush(w, 2 * PER_SEG + 5));
    checkpoint_at(s, 2 * PER_SEG + 9); /* advance the store without logging this operation */
    close_both(s, w);
    s = open_store(0);
    w = open_wal(s);
    T_CHECK(!seg_exists(1));
    T_CHECK(!seg_exists(2));
    T_CHECK(seg_exists(3));
    T_EQ(seg_header(4).prev_lsn, 2 * PER_SEG + 5);
    u64 cnt = 0;
    check_iter(w, 2 * PER_SEG + 10, BIG, &cnt);
    T_EQ(cnt, 0);
    append_rec(w, 2 * PER_SEG + 10, 8);
    close_both(s, w);
    teardown();
}

static void test_no_anchor(void)
{
    /* Checkpoint at 50. Segment 4 spans 46 through 60 and is the anchor. */
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    append_range(w, 1, 4 * PER_SEG + 10, BIG);
    T_OK(ps_wal_flush(w, 4 * PER_SEG + 10));
    checkpoint_at(s, 50);
    close_both(s, w);
    for (u64 seq = 1; seq <= 4; seq++) {
        unlink_seg(seq);
    }
    s = open_store(0);
    ps_wal *w2 = NULL;
    T_ERR(try_open_wal(s, &w2), EBADMSG);
    T_CHECK(seg_exists(5));
    ps_close(s);
    teardown();

    s = setup();
    w = open_wal(s);
    append_range(w, 1, 4 * PER_SEG + 10, BIG);
    T_OK(ps_wal_flush(w, 4 * PER_SEG + 10));
    checkpoint_at(s, 50);
    close_both(s, w);
    unlink_seg(4);
    s = open_store(0);
    T_ERR(try_open_wal(s, &w2), EBADMSG);
    ps_close(s);
    teardown();
}

static void test_lost_log_nonzero_durable(void)
{
    ps_store *s = setup();
    ps_wal *w = open_wal(s);
    append_range(w, 1, 5, 0);
    T_OK(ps_wal_flush(w, 5));
    checkpoint_at(s, 5);
    close_both(s, w);
    unlink_seg(1);
    s = open_store(0);
    ps_wal *w2 = NULL;
    T_ERR(try_open_wal(s, &w2), EBADMSG);
    ps_close(s);
    teardown();
}

static void test_tmp_deleted(void)
{
    build_segments(5);
    ps_file f;
    T_OK(vfs->open(vfs->ctx, WALDIR "/0000000000000009.wal.tmp", PS_VFS_CREATE, &f));
    vfs->close(vfs->ctx, &f);
    T_OK(vfs->open(vfs->ctx, WALDIR "/0000000000000002.wal.tmp", PS_VFS_CREATE, &f));
    vfs->close(vfs->ctx, &f);
    T_EQ(count_files().tmps, 2);
    ps_store *s = open_store(0);
    ps_wal *w = open_wal(s);
    T_EQ(count_files().tmps, 0);
    T_CHECK(!file_exists(WALDIR "/0000000000000009.wal.tmp"));
    T_CHECK(seg_exists(2)); /* stale temp file was removed before publishing */
    T_EQ(check_iter(w, 1, BIG, NULL), 5);
    close_both(s, w);
    teardown();
}

static void test_repeated_reopen_no_appends(void)
{
    build_segments(PER_SEG + 3);
    for (u32 i = 0; i < 3; i++) {
        ps_store *s = open_store(0);
        ps_wal *w = open_wal(s);
        u64 cnt = 0;
        T_EQ(check_iter(w, 1, BIG, &cnt), PER_SEG + 3);
        T_EQ(cnt, PER_SEG + 3);
        /* Empty segments can share the same prev_lsn across reopens. */
        T_EQ(count_files().segs, 3 + i);
        T_EQ(seg_header(3 + i).prev_lsn, PER_SEG + 3);
        if (i > 0) {
            T_EQ(seg_size(2 + i), PS_WAL_SEG_HEADER_SIZE);
        }
        close_both(s, w);
        if (i == 1) {
            fault_vfs_reboot(fv, FAULT_RANDOM_UNSYNCED);
        }
    }
    teardown();
}

static void test_seal_failure(void)
{
    /* Inject errors one at a time. A clean retry must recover the same records. */
    build_segments(PER_SEG + 3);
    bool saw_ok = false;
    for (u64 k = 1; k <= 40 && !saw_ok; k++) {
        ps_store *s = open_store(0);
        fault_vfs_fail_at(fv, fault_vfs_points(fv) + k, EIO);
        ps_wal *w = NULL;
        const int rv = try_open_wal(s, &w);
        fault_vfs_fail_at(fv, 0, 0);
        if (rv == 0) {
            saw_ok = true;
            T_EQ(check_iter(w, 1, BIG, NULL), PER_SEG + 3);
            close_both(s, w);
            break;
        }
        T_ERR(rv, EIO);
        ps_close(s);
        T_CHECK(w == NULL);
        s = open_store(0);
        w = open_wal(s);
        T_EQ(check_iter(w, 1, BIG, NULL), PER_SEG + 3);
        close_both(s, w);
    }
    T_CHECK(saw_ok);
    teardown();

    /* The seal fsync specifically: the open fails and the tail is not sealed. */
    ps_store *s = setup();
    ps_wal_config c = wal_config();
    c.buffer_size = 0;
    ps_wal *w = open_wal_cfg(s, &c);
    append_range(w, 1, 4, 0);
    T_OK(ps_wal_flush(w, 4));
    close_both(s, w);
    s = open_store(0);
    /* Points of ps_wal_open here: mkdir (EEXIST), fsync_dir(parent), truncate, fsync, ... */
    fault_vfs_fail_at(fv, fault_vfs_points(fv) + 4, EIO);
    T_ERR(try_open_wal(s, &w), EIO);
    fault_vfs_fail_at(fv, 0, 0);
    T_EQ(count_files().segs, 1); /* nothing published */
    ps_close(s);
    s = open_store(0);
    w = open_wal(s);
    T_EQ(check_iter(w, 1, 0, NULL), 4);
    close_both(s, w);
    teardown();
}

static void test_double_crash_no_resurrection(void)
{
    for (u64 seed = 1; seed <= 40; seed++) {
        ps_store *s = setup_seed(seed);
        ps_wal_config c = wal_config();
        c.buffer_size = 0;
        ps_wal *w = open_wal_cfg(s, &c);
        append_range(w, 1, 5, 0);
        T_OK(ps_wal_flush(w, 5));
        append_range(w, 6, 8, 0);
        fault_vfs_reboot(fv, FAULT_RANDOM_UNSYNCED);
        close_both(s, w);

        s = open_store(0);
        w = open_wal_cfg(s, &c);
        const u64 p = check_iter(w, 1, 0, NULL);
        T_CHECK(p >= 5 && p <= 8);
        /* Keep lengths the same so stale payloads could occupy the same offsets. */
        for (u64 lsn = p + 1; lsn <= 10; lsn++) {
            T_OK(append_tagged(w, lsn, rec_len(lsn), 1));
        }
        if (seed % 2 == 0) {
            T_OK(ps_wal_flush(w, p + 1));
        }
        fault_vfs_reboot(fv, FAULT_RANDOM_UNSYNCED);
        close_both(s, w);

        s = open_store(0);
        w = open_wal_cfg(s, &c);
        const u64 k = check_iter_tagged(w, 1, 0, p, 1, NULL);
        T_CHECK(k >= p && k <= 10);
        if (seed % 2 == 0) {
            T_CHECK(k >= p + 1);
        }
        close_both(s, w);
        teardown();
    }
}

/* Append enough records to rotate and keep the highest successful flush as a recovery floor. */
static u64 crash_workload(ps_wal *w, u64 *acked)
{
    const u64 n = 3 * PER_SEG + 2;
    for (u64 lsn = 1; lsn <= n; lsn++) {
        const u32 len = lsn % 5 == 0 ? BIG : rec_len(lsn) * 10u;
        if (append_tagged(w, lsn, len, 0) != 0) {
            return lsn - 1;
        }
        if (lsn % 4 == 0) {
            if (ps_wal_flush(w, lsn) != 0) {
                return lsn;
            }
            *acked = lsn;
        }
    }
    return n;
}

static u64 crash_check(ps_wal *w, u64 acked)
{
    ps_wal_iter *it = NULL;
    T_OK(ps_wal_iter_open(w, 1, &it));
    u64 want = 1;
    for (;;) {
        u64 lsn = 0;
        const void *buf = NULL;
        usize len = 0;
        const int rv = ps_wal_iter_next(it, &lsn, &buf, &len);
        if (rv == ENOENT) {
            break;
        }
        T_OK(rv);
        T_EQ(lsn, want);
        const u32 l = lsn % 5 == 0 ? BIG : rec_len(lsn) * 10u;
        T_EQ(len, l);
        u8 exp[MAXREC + 8];
        fill_payload(exp, lsn, l, 0);
        T_MEMEQ(buf, exp, l);
        want++;
    }
    ps_wal_iter_close(it);
    T_CHECK(want - 1 >= acked);
    return want - 1;
}

static u64 crash_every_point(usize buffer_size)
{
    ps_wal_config c = wal_config();
    c.buffer_size = buffer_size;
    u64 total = 0;
    for (u64 p = 1;; p++) {
        ps_store *s = setup_seed(p);
        ps_wal *w = open_wal_cfg(s, &c);
        const u64 base = fault_vfs_points(fv);
        fault_vfs_crash_at(fv, base + p);
        u64 acked = 0;
        crash_workload(w, &acked);
        const bool crashed = fault_vfs_crashed(fv);
        fault_vfs_reboot(fv, p % 3 == 0 ? FAULT_DROP_UNSYNCED : FAULT_RANDOM_UNSYNCED);
        close_both(s, w);

        /* Reopen, crashing at point q of the WAL open (0 = no crash). */
        u64 q = p % 7;
        u64 prefix = 0;
        for (;;) {
            s = open_store(0);
            if (q != 0) {
                fault_vfs_crash_at(fv, fault_vfs_points(fv) + q);
            }
            w = NULL;
            const int rv = try_open_wal_cfg(s, &c, &w);
            if (rv == 0) {
                fault_vfs_crash_at(fv, 0);
                prefix = crash_check(w, acked);
                close_both(s, w);
                break;
            }
            T_ERR(rv, EIO);
            T_CHECK(fault_vfs_crashed(fv));
            ps_close(s);
            fault_vfs_reboot(fv, FAULT_RANDOM_UNSYNCED);
            q = 0;
        }
        /* And once more after a power loss: the sealed prefix stays exactly. */
        fault_vfs_reboot(fv, FAULT_RANDOM_UNSYNCED);
        s = open_store(0);
        w = open_wal_cfg(s, &c);
        T_EQ(crash_check(w, acked), prefix);
        close_both(s, w);
        teardown();
        total++;
        if (!crashed) {
            break;
        }
    }
    return total;
}

static void test_crash_every_point(void)
{
    T_CHECK(crash_every_point(0) > 100);
    T_CHECK(crash_every_point(8192) > 20);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_reopen_lists_records);
    T_RUN(test_iter_spans_segments);
    T_RUN(test_iter_fresh_wal_empty);
    T_RUN(test_iter_invalid_after_append);
    T_RUN(test_iter_holds_off_truncate);
    T_RUN(test_torn_tail);
    T_RUN(test_corrupt_sealed_segment);
    T_RUN(test_corrupt_newest_is_tail);
    T_RUN(test_broken_chain);
    T_RUN(test_missing_middle_segment);
    T_RUN(test_obsolete_gap_tolerated);
    T_RUN(test_obsolete_corrupt_ignored);
    T_RUN(test_anchor_is_newest);
    T_RUN(test_no_anchor);
    T_RUN(test_lost_log_nonzero_durable);
    T_RUN(test_tmp_deleted);
    T_RUN(test_repeated_reopen_no_appends);
    T_RUN(test_seal_failure);
    T_RUN(test_double_crash_no_resurrection);
    T_RUN(test_crash_every_point);
    return t_done();
}
