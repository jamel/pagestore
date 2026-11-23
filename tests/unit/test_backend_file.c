#include "backend_file.h"
#include "crc32c.h"
#include "fault_vfs.h"
#include "format.h"
#include "pagestore/backend.h"
#include "test.h"
#include "util.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#define PS 4096u
#define DIR "/s"

static fault_vfs *fv;
static const ps_vfs *v;

static void setup(u64 seed)
{
    fv = fault_vfs_create(seed);
    v = fault_vfs_get(fv);
}

static void teardown(void)
{
    fault_vfs_destroy(fv);
    fv = NULL;
    v = NULL;
}

/* An open error closes and frees the backend before returning. */
static int be_open_ps(ps_backend *be, bool create, u32 page_size, ps_backend_info *info)
{
    T_OK(ps_file_backend_new(DIR, v, be));
    const int rv = be->open(be->ctx, create, page_size, info);
    if (rv != 0) {
        be->close(be->ctx);
    }
    return rv;
}

static int be_open(ps_backend *be, bool create, ps_backend_info *info)
{
    return be_open_ps(be, create, PS, info);
}

static void be_close(ps_backend *be)
{
    be->close(be->ctx);
}

static bool file_exists(const char *path)
{
    ps_file f;
    const int rv = v->open(v->ctx, path, 0, &f);
    if (rv == 0) {
        v->close(v->ctx, &f);
        return true;
    }
    T_ERR(rv, ENOENT);
    return false;
}

static u64 file_size(const char *path)
{
    ps_file f;
    T_OK(v->open(v->ctx, path, 0, &f));
    u64 sz = 0;
    T_OK(v->size(v->ctx, &f, &sz));
    v->close(v->ctx, &f);
    return sz;
}

static void check_info(const ps_backend_info *info, u32 page_size, u32 ng, u64 seq, u64 lsn)
{
    T_EQ(info->page_size, page_size);
    T_EQ(info->num_groups, ng);
    T_EQ(info->seq, seq);
    T_EQ(info->durable_lsn, lsn);
}

/* Check every byte so a partly written page cannot pass the fill check. */
static u8 page_byte(ps_backend *be, u32 id)
{
    static u8 buf[PS];
    memset(buf, 0xCC, sizeof(buf));
    T_OK(be->read_page(be->ctx, id, buf));
    for (usize i = 1; i < PS; i++) {
        if (buf[i] != buf[0]) {
            t_fail(
                __FILE__,
                __LINE__,
                "page %u byte %zu = %#x, byte 0 = %#x",
                id,
                i,
                buf[i],
                buf[0]);
        }
    }
    return buf[0];
}

/*
 * *fatal is false when an earlier error kept us from calling commit. Like
 * the store, abort after a non-fatal error that follows a successful begin.
 */
static int checkpoint_ng(
    ps_backend *be,
    u64 lsn,
    u32 ng,
    const u32 *ids,
    const u8 *fills,
    u32 n,
    bool *fatal)
{
    static u8 page[PS];
    *fatal = false;
    int rv = be->ckpt_begin(be->ctx, lsn, ng, n);
    if (rv != 0) {
        return rv;
    }
    for (u32 i = 0; i < n && rv == 0; i++) {
        memset(page, fills[i], sizeof(page));
        rv = be->ckpt_write(be->ctx, ids[i], page);
    }
    if (rv == 0) {
        rv = be->ckpt_commit(be->ctx, fatal);
    }
    if (rv != 0 && !*fatal) {
        be->ckpt_abort(be->ctx);
    }
    return rv;
}

static int checkpoint(ps_backend *be, u64 lsn, const u32 *ids, const u8 *fills, u32 n, bool *fatal)
{
    return checkpoint_ng(be, lsn, 1, ids, fills, n, fatal);
}

static void checkpoint_ok(ps_backend *be, u64 lsn, const u32 *ids, const u8 *fills, u32 n)
{
    bool fatal = true;
    T_OK(checkpoint(be, lsn, ids, fills, n, &fatal));
    T_CHECK(!fatal);
}

static const u32 a_ids[] = {1, 2, 3};
static const u8 a_fills[] = {0xA1, 0xA2, 0xA3};
static const u32 b_ids[] = {2, 3, 4, 5};
static const u8 b_fills[] = {0xB2, 0xB3, 0xB4, 0xB5};

static void ckpt_a(ps_backend *be)
{
    checkpoint_ok(be, 1, a_ids, a_fills, 3);
}

static int ckpt_b(ps_backend *be, bool *fatal)
{
    return checkpoint(be, 2, b_ids, b_fills, 4, fatal);
}

/* Compare the entire image so a mix of the two checkpoints fails. */
static void check_image_a_or_b(ps_backend *be, const ps_backend_info *info)
{
    u8 got[5];
    for (u32 i = 0; i < 5; i++) {
        got[i] = page_byte(be, i + 1);
    }
    static const u8 img_a[5] = {0xA1, 0xA2, 0xA3, 0, 0};
    static const u8 img_b[5] = {0xA1, 0xB2, 0xB3, 0xB4, 0xB5};
    if (info->durable_lsn == 1) {
        T_EQ(info->seq, 1);
        T_MEMEQ(got, img_a, 5);
    } else {
        T_EQ(info->durable_lsn, 2);
        T_EQ(info->seq, 2);
        T_MEMEQ(got, img_b, 5);
    }
    T_EQ(info->num_groups, 1);
    T_EQ(info->page_size, PS);
}

/* Install a complete durable journal without applying it to the data file. */
static void write_journal(u64 seq, u64 lsn, const u32 *ids, const u8 *fills, u32 n)
{
    const ps_journal_header jh =
        {.page_size = PS, .num_groups = 1, .count = n, .seq = seq, .lsn = lsn};
    ps_file f;
    T_OK(v->open(v->ctx, DIR "/journal", PS_VFS_CREATE, &f));
    T_OK(v->truncate(v->ctx, &f, 0));
    u8 hdr[PS_JOURNAL_HEADER_SIZE];
    ps_journal_header_encode(&jh, hdr);
    T_OK(v->pwrite(v->ctx, &f, hdr, sizeof(hdr), 0));
    u32 crc = ps_crc32c(0, hdr, sizeof(hdr));
    static u8 entry[PS_JOURNAL_ENTRY_HEADER_SIZE + PS];
    for (u32 i = 0; i < n; i++) {
        ps_journal_entry_header_encode(ids[i], entry);
        memset(entry + PS_JOURNAL_ENTRY_HEADER_SIZE, fills[i], PS);
        T_OK(v->pwrite(v->ctx, &f, entry, sizeof(entry), ps_journal_entry_off(&jh, i)));
        crc = ps_crc32c(crc, entry, sizeof(entry));
    }
    u8 trailer[PS_JOURNAL_TRAILER_SIZE];
    ps_journal_trailer_encode(crc, trailer);
    T_OK(v->pwrite(v->ctx, &f, trailer, sizeof(trailer), ps_journal_entry_off(&jh, n)));
    T_OK(v->fsync(v->ctx, &f));
    v->close(v->ctx, &f);
    T_EQ(file_size(DIR "/journal"), ps_journal_size(&jh));
}

static void truncate_file(const char *path, u64 size)
{
    ps_file f;
    T_OK(v->open(v->ctx, path, 0, &f));
    T_OK(v->truncate(v->ctx, &f, size));
    T_OK(v->fsync(v->ctx, &f));
    v->close(v->ctx, &f);
}

/* Leave image A durable and the backend closed for the next open. */
static void setup_image_a(u64 seed)
{
    setup(seed);
    ps_backend be;
    ps_backend_info info;
    T_OK(be_open(&be, true, &info));
    ckpt_a(&be);
    be_close(&be);
}

/* ---- tests ---- */

static void test_create_then_open(void)
{
    setup(1);
    ps_backend be;
    ps_backend_info info;
    T_OK(be_open(&be, true, &info));
    check_info(&info, PS, 1, 0, 0);
    T_CHECK(file_exists(DIR "/data"));
    T_CHECK(file_exists(DIR "/journal"));
    T_CHECK(file_exists(DIR "/LOCK"));
    T_CHECK(!file_exists(DIR "/data.creating"));
    T_EQ(file_size(DIR "/journal"), 0);
    T_EQ(page_byte(&be, 0), 0);
    be_close(&be);

    memset(&info, 0xFF, sizeof(info));
    T_OK(be_open(&be, false, &info));
    check_info(&info, PS, 1, 0, 0);
    be_close(&be);

    /* A formatted seq 0 store is already an existing store. */
    T_OK(be_open(&be, true, &info));
    check_info(&info, PS, 1, 0, 0);
    be_close(&be);

    T_OK(be_open_ps(&be, false, 8192, &info));
    check_info(&info, PS, 1, 0, 0);
    be_close(&be);
    teardown();

    setup(2);
    T_ERR(be_open_ps(&be, true, 1000, &info), EINVAL);
    T_ERR(be_open_ps(&be, true, 2 * PS_PAGE_SIZE_MAX, &info), EINVAL);
    T_OK(be_open_ps(&be, true, 0, &info));
    check_info(&info, 4096, 1, 0, 0);
    be_close(&be);
    teardown();

    setup(3);
    T_OK(be_open_ps(&be, true, 16384, &info));
    check_info(&info, 16384, 1, 0, 0);
    be_close(&be);
    T_OK(be_open_ps(&be, false, 0, &info));
    check_info(&info, 16384, 1, 0, 0);
    be_close(&be);
    teardown();
}

static void test_open_missing_enoent(void)
{
    setup(1);
    ps_backend be;
    ps_backend_info info;
    T_ERR(be_open(&be, false, &info), ENOENT);
    T_CHECK(!file_exists(DIR));
    fault_vfs_mkdir_durable(fv, DIR);
    T_ERR(be_open(&be, false, &info), ENOENT);
    T_CHECK(!file_exists(DIR "/data"));
    T_CHECK(!file_exists(DIR "/journal"));
    /* A failed format can leave data.creating without publishing data. */
    ps_file f;
    T_OK(v->open(v->ctx, DIR "/data.creating", PS_VFS_CREATE, &f));
    v->close(v->ctx, &f);
    T_ERR(be_open(&be, false, &info), ENOENT);
    T_OK(be_open(&be, true, &info));
    check_info(&info, PS, 1, 0, 0);
    T_CHECK(!file_exists(DIR "/data.creating"));
    be_close(&be);
    teardown();
}

static void test_lock_ebusy(void)
{
    setup(1);
    ps_backend be1;
    ps_backend be2;
    ps_backend_info info;
    T_OK(be_open(&be1, true, &info));
    T_ERR(be_open(&be2, false, &info), EBUSY);
    T_ERR(be_open(&be2, true, &info), EBUSY);
    be_close(&be1);
    T_OK(be_open(&be2, false, &info));
    be_close(&be2);
    teardown();
}

static void test_checkpoint_roundtrip(void)
{
    setup(1);
    ps_backend be;
    ps_backend_info info;
    T_OK(be_open(&be, true, &info));
    const u32 ids[] = {1, 7};
    T_OK(be.ckpt_begin(be.ctx, 5, 1, 2));
    static u8 page[PS];
    memset(page, 0x11, sizeof(page));
    T_OK(be.ckpt_write(be.ctx, 1, page));
    memset(page, 0x77, sizeof(page));
    T_OK(be.ckpt_write(be.ctx, 7, page));
    memset(page, 0, sizeof(page)); /* reuse the caller buffer before commit */
    bool fatal = true;
    T_OK(be.ckpt_commit(be.ctx, &fatal));
    T_CHECK(!fatal);
    T_EQ(page_byte(&be, 7), 0x77);
    T_EQ(page_byte(&be, 1), 0x11);
    T_EQ(page_byte(&be, 3), 0);
    T_EQ(page_byte(&be, 100), 0);
    T_EQ(file_size(DIR "/journal"), 0);
    T_EQ(file_size(DIR "/data"), 9 * (u64) PS);
    be_close(&be);

    T_OK(be_open(&be, false, &info));
    check_info(&info, PS, 1, 1, 5);
    T_EQ(page_byte(&be, 7), 0x77);
    T_EQ(page_byte(&be, 1), 0x11);

    const u8 fills2[] = {0x12};
    T_OK(checkpoint_ng(&be, 9, 2, ids, fills2, 1, &fatal));
    T_CHECK(!fatal);
    T_OK(checkpoint_ng(&be, 10, 2, NULL, NULL, 0, &fatal));
    T_CHECK(!fatal);
    be_close(&be);
    T_OK(be_open(&be, false, &info));
    check_info(&info, PS, 2, 3, 10);
    T_EQ(page_byte(&be, 1), 0x12);
    T_EQ(page_byte(&be, 7), 0x77);
    be_close(&be);
    teardown();
}

static void test_abort_then_checkpoint(void)
{
    setup(1);
    ps_backend be;
    ps_backend_info info;
    T_OK(be_open(&be, true, &info));
    ckpt_a(&be);
    static u8 page[PS];
    memset(page, 0xEE, sizeof(page));
    T_OK(be.ckpt_begin(be.ctx, 2, 1, 2));
    T_OK(be.ckpt_write(be.ctx, 1, page));
    be.ckpt_abort(be.ctx);
    T_EQ(file_size(DIR "/journal"), 0);
    T_EQ(page_byte(&be, 1), 0xA1);
    bool fatal = true;
    T_OK(ckpt_b(&be, &fatal));
    T_CHECK(!fatal);
    be_close(&be);
    T_OK(be_open(&be, false, &info));
    check_info(&info, PS, 1, 2, 2);
    check_image_a_or_b(&be, &info);
    be_close(&be);
    teardown();
}

static void test_max_page_id_offset(void)
{
    setup(1);
    ps_backend be;
    ps_backend_info info;
    T_OK(be_open(&be, true, &info));
    const u32 ids[] = {0xFFFFFFFEu, 1};
    const u8 fills[] = {0x5E, 0x01};
    checkpoint_ok(&be, 1, ids, fills, 2);
    T_EQ(page_byte(&be, 0xFFFFFFFEu), 0x5E);
    T_EQ(page_byte(&be, 1), 0x01);
    T_EQ(page_byte(&be, 0), 0);
    T_EQ(file_size(DIR "/data"), (u64) 0xFFFFFFFFu * 4096 + 4096);
    be_close(&be);
    write_journal(2, 2, ids, (const u8[]) {0x6E, 0x02}, 2);
    T_OK(be_open(&be, false, &info));
    check_info(&info, PS, 1, 2, 2);
    T_EQ(page_byte(&be, 0xFFFFFFFEu), 0x6E);
    T_EQ(page_byte(&be, 1), 0x02);
    T_EQ(file_size(DIR "/data"), (u64) 0xFFFFFFFFu * 4096 + 4096);
    be_close(&be);
    teardown();
}

static void test_journal_enospc_nonfatal(void)
{
    setup_image_a(1);
    ps_backend be;
    ps_backend_info info;
    T_OK(be_open(&be, false, &info));
    /* Room for less than one journal entry beyond the data file. */
    fault_vfs_enospc_after(fv, file_size(DIR "/data") + 1000);
    bool fatal = true;
    T_ERR(ckpt_b(&be, &fatal), ENOSPC);
    T_CHECK(!fatal);
    T_EQ(page_byte(&be, 2), 0xA2);
    be_close(&be);

    T_OK(be_open(&be, false, &info));
    check_info(&info, PS, 1, 1, 1);
    check_image_a_or_b(&be, &info);
    fault_vfs_enospc_after(fv, UINT64_MAX);
    T_OK(ckpt_b(&be, &fatal));
    T_CHECK(!fatal);
    be_close(&be);
    T_OK(be_open(&be, false, &info));
    check_info(&info, PS, 1, 2, 2);
    check_image_a_or_b(&be, &info);
    be_close(&be);
    teardown();
}

/* Measure the fault points in checkpoint B on a clean run from image A. */
static u64 ckpt_b_points(void)
{
    setup_image_a(1);
    ps_backend be;
    ps_backend_info info;
    T_OK(be_open(&be, false, &info));
    const u64 p0 = fault_vfs_points(fv);
    bool fatal = true;
    T_OK(ckpt_b(&be, &fatal));
    const u64 n = fault_vfs_points(fv) - p0;
    be_close(&be);
    teardown();
    return n;
}

static void test_fsync_failure_fatal(void)
{
    /* Point 1 syncs the directory and point 2 writes the journal. Fail its fsync. */
    setup_image_a(1);
    ps_backend be;
    ps_backend_info info;
    T_OK(be_open(&be, false, &info));
    fault_vfs_fail_at(fv, fault_vfs_points(fv) + 3, EIO);
    bool fatal = false;
    T_ERR(ckpt_b(&be, &fatal), EIO);
    T_CHECK(fatal);
    T_ERR(be.ckpt_begin(be.ctx, 3, 1, 0), EIO);
    be_close(&be);
    teardown();

    /*
     * The directory sync and the journal write can be retried. Failures
     * starting at the journal fsync require reopening, except the final
     * truncation after commit. Reopen must yield a whole checkpoint.
     */
    const u64 n = ckpt_b_points();
    T_CHECK(n >= 6);
    for (u64 k = 1; k <= n; k++) {
        setup_image_a(1);
        T_OK(be_open(&be, false, &info));
        fault_vfs_fail_at(fv, fault_vfs_points(fv) + k, EIO);
        fatal = false;
        const int rv = ckpt_b(&be, &fatal);
        if (k == n) {
            T_OK(rv);
            T_CHECK(!fatal);
        } else {
            T_ERR(rv, EIO);
            T_EQ(fatal, k >= 3);
        }
        be_close(&be);
        T_OK(be_open(&be, false, &info));
        check_image_a_or_b(&be, &info);
        if (k >= 3) {
            T_EQ(info.seq, 2); /* even an fsync error leaves replayable bytes before a power loss */
        }
        be_close(&be);
        teardown();
    }
}

static void test_recovery_table(void)
{
    ps_backend be;
    ps_backend_info info;
    const u32 ids[] = {2, 9};
    const u8 fills[] = {0xC2, 0xC9};

    for (u64 jseq = 1; jseq <= 2; jseq++) {
        setup_image_a(1);
        write_journal(jseq, 7, ids, fills, 2);
        T_OK(be_open(&be, false, &info));
        check_info(&info, PS, 1, jseq, 7);
        T_EQ(page_byte(&be, 1), 0xA1);
        T_EQ(page_byte(&be, 2), 0xC2);
        T_EQ(page_byte(&be, 9), 0xC9);
        T_EQ(file_size(DIR "/journal"), 0);
        be_close(&be);
        T_OK(be_open(&be, false, &info));
        check_info(&info, PS, 1, jseq, 7);
        be_close(&be);
        teardown();
    }

    /* Crash after journal fsync, at the first in-place write. */
    setup_image_a(1);
    T_OK(be_open(&be, false, &info));
    fault_vfs_crash_at(fv, fault_vfs_points(fv) + 4);
    bool fatal = false;
    T_ERR(ckpt_b(&be, &fatal), EIO);
    T_CHECK(fatal);
    be_close(&be);
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    T_CHECK(file_size(DIR "/journal") > 0);
    T_OK(be_open(&be, false, &info));
    check_info(&info, PS, 1, 2, 2);
    check_image_a_or_b(&be, &info);
    be_close(&be);
    teardown();

    setup_image_a(1);
    write_journal(0, 7, ids, fills, 2);
    T_OK(be_open(&be, false, &info));
    check_info(&info, PS, 1, 1, 1);
    T_EQ(page_byte(&be, 2), 0xA2);
    T_EQ(page_byte(&be, 9), 0);
    /* Leave the stale journal in place for the next checkpoint. */
    T_OK(ckpt_b(&be, &fatal));
    be_close(&be);
    T_OK(be_open(&be, false, &info));
    check_info(&info, PS, 1, 2, 2);
    check_image_a_or_b(&be, &info);
    be_close(&be);
    teardown();

    for (u32 variant = 0; variant < 3; variant++) {
        setup_image_a(1);
        write_journal(5, 7, ids, fills, 2);
        if (variant == 0) {
            truncate_file(DIR "/journal", file_size(DIR "/journal") - 1);
        } else if (variant == 1) {
            T_OK(fault_vfs_corrupt(fv, DIR "/journal", PS_JOURNAL_HEADER_SIZE + 100, 0x01));
        } else {
            T_OK(fault_vfs_corrupt(fv, DIR "/journal", 0, 0x01));
        }
        T_OK(be_open(&be, false, &info));
        check_info(&info, PS, 1, 1, 1);
        T_EQ(page_byte(&be, 2), 0xA2);
        T_EQ(page_byte(&be, 9), 0);
        be_close(&be);
        teardown();
    }

    /* A sound journal can reconstruct the data header as well as the pages. */
    setup_image_a(1);
    write_journal(2, 7, ids, fills, 2);
    T_OK(fault_vfs_corrupt(fv, DIR "/data", 3, 0x40));
    T_OK(be_open(&be, false, &info));
    check_info(&info, PS, 1, 2, 7);
    T_EQ(page_byte(&be, 2), 0xC2);
    T_EQ(page_byte(&be, 1), 0xA1);
    be_close(&be);
    T_OK(be_open(&be, false, &info));
    check_info(&info, PS, 1, 2, 7);
    be_close(&be);
    teardown();

    setup_image_a(1);
    write_journal(2, 7, ids, fills, 2);
    truncate_file(DIR "/journal", file_size(DIR "/journal") - 8);
    T_OK(fault_vfs_corrupt(fv, DIR "/data", 3, 0x40));
    T_ERR(be_open(&be, false, &info), EBADMSG);
    T_ERR(be_open(&be, true, &info), EBADMSG);
    teardown();

    setup_image_a(1);
    T_OK(fault_vfs_corrupt(fv, DIR "/data", 3, 0x40));
    T_ERR(be_open(&be, false, &info), EBADMSG);
    teardown();
}

static void test_journal_creation_sync_retry(void)
{
    setup_image_a(1);
    T_OK(v->unlink(v->ctx, DIR "/journal"));
    T_OK(v->fsync_dir(v->ctx, DIR));

    ps_backend be;
    ps_backend_info info;
    /* Recreating the journal doesn't sync the directory, so the open does no syncs. */
    const u64 p0 = fault_vfs_points(fv);
    T_OK(be_open(&be, false, &info));
    T_EQ(fault_vfs_points(fv) - p0, 1);

    /* A failed directory sync leaves the checkpoint retryable. */
    fault_vfs_fail_at(fv, fault_vfs_points(fv) + 1, EIO);
    bool fatal = true;
    T_ERR(ckpt_b(&be, &fatal), EIO);
    T_CHECK(!fatal);

    /* The retry syncs. Crash at the second in-place write, after the journal file sync. */
    fault_vfs_crash_at(fv, fault_vfs_points(fv) + 5);
    T_ERR(ckpt_b(&be, &fatal), EIO);
    T_CHECK(fatal);
    be_close(&be);
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);

    T_OK(be_open(&be, false, &info));
    check_info(&info, PS, 1, 2, 2);
    check_image_a_or_b(&be, &info);
    be_close(&be);
    teardown();
}

/* k counts from the start of checkpoint B, after image A is durable. */
static bool crash_ckpt_run(u64 k, fault_mode mode, u64 seed)
{
    setup_image_a(seed);
    ps_backend be;
    ps_backend_info info;
    T_OK(be_open(&be, false, &info));
    fault_vfs_crash_at(fv, fault_vfs_points(fv) + k);
    bool fatal = false;
    const int rv = ckpt_b(&be, &fatal);
    const bool crashed = fault_vfs_crashed(fv);
    be_close(&be);
    if (crashed) {
        fault_vfs_reboot(fv, mode);
    } else {
        T_OK(rv);
        fault_vfs_reboot(fv, mode);
        T_OK(be_open(&be, false, &info));
        check_info(&info, PS, 1, 2, 2);
        check_image_a_or_b(&be, &info);
        be_close(&be);
        teardown();
        return false;
    }
    T_OK(be_open(&be, false, &info));
    check_image_a_or_b(&be, &info);
    const u32 ids[] = {1};
    const u8 fills[] = {0xD1};
    checkpoint_ok(&be, 3, ids, fills, 1);
    be_close(&be);
    T_OK(be_open(&be, false, &info));
    T_EQ(info.durable_lsn, 3);
    T_EQ(page_byte(&be, 1), 0xD1);
    be_close(&be);
    teardown();
    return true;
}

static void test_crash_every_point_checkpoint(void)
{
    u64 k = 1;
    for (;; k++) {
        const bool crashed = crash_ckpt_run(k, FAULT_DROP_UNSYNCED, k);
        for (u32 s = 0; s < 4 * t_scale(); s++) {
            T_EQ(crash_ckpt_run(k, FAULT_RANDOM_UNSYNCED, k * 1000 + s), crashed);
        }
        if (!crashed) {
            break;
        }
    }
    T_CHECK(k > 5);
}

/* Returns false if format completes before reaching point k. */
static bool crash_format_run(u64 k, fault_mode mode, u64 seed)
{
    setup(seed);
    ps_backend be;
    ps_backend_info info;
    fault_vfs_crash_at(fv, fault_vfs_points(fv) + k);
    T_OK(ps_file_backend_new(DIR, v, &be));
    const int rv = be.open(be.ctx, true, PS, &info);
    const bool crashed = fault_vfs_crashed(fv);
    be_close(&be);
    if (!crashed) {
        T_OK(rv);
        check_info(&info, PS, 1, 0, 0);
        fault_vfs_reboot(fv, mode);
        T_OK(be_open(&be, false, &info));
        check_info(&info, PS, 1, 0, 0);
        be_close(&be);
        teardown();
        return false;
    }
    T_NE(rv, 0);
    fault_vfs_reboot(fv, mode);
    const int rv2 = be_open(&be, false, &info);
    if (rv2 == 0) {
        check_info(&info, PS, 1, 0, 0);
        be_close(&be);
    } else {
        T_ERR(rv2, ENOENT);
    }
    T_OK(be_open(&be, true, &info));
    check_info(&info, PS, 1, 0, 0);
    ckpt_a(&be);
    be_close(&be);
    T_OK(be_open(&be, false, &info));
    check_info(&info, PS, 1, 1, 1);
    be_close(&be);
    teardown();
    return true;
}

static void test_crash_every_point_format(void)
{
    u64 k = 1;
    for (;; k++) {
        const bool crashed = crash_format_run(k, FAULT_DROP_UNSYNCED, k);
        for (u32 s = 0; s < 4 * t_scale(); s++) {
            T_EQ(crash_format_run(k, FAULT_RANDOM_UNSYNCED, k * 1000 + s), crashed);
        }
        if (!crashed) {
            break;
        }
    }
    T_CHECK(k > 5);
}

/* read_page from another thread while checkpoints run. */
typedef struct reader_arg {
    ps_backend *be;
    atomic_bool stop;
    u64 reads;
} reader_arg;

static void *reader_main(void *p)
{
    reader_arg *a = p;
    static u8 buf[PS];
    while (!atomic_load(&a->stop)) {
        T_OK(a->be->read_page(a->be->ctx, 1, buf));
        T_EQ(buf[0], 0xA1);
        T_EQ(buf[PS - 1], 0xA1);
        a->reads++;
    }
    return NULL;
}

static void test_read_concurrent_with_checkpoint(void)
{
    setup_image_a(1);
    ps_backend be;
    ps_backend_info info;
    T_OK(be_open(&be, false, &info));
    reader_arg a = {.be = &be, .reads = 0};
    atomic_init(&a.stop, false);
    pthread_t th;
    T_EQ(pthread_create(&th, NULL, reader_main, &a), 0);
    const u32 ids[] = {2, 3};
    for (u32 i = 0; i < 50; i++) {
        const u8 fills[] = {(u8) i, (u8) (i + 1)};
        checkpoint_ok(&be, 2 + i, ids, fills, 2);
    }
    atomic_store(&a.stop, true);
    T_EQ(pthread_join(th, NULL), 0);
    be_close(&be);
    teardown();
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_create_then_open);
    T_RUN(test_open_missing_enoent);
    T_RUN(test_lock_ebusy);
    T_RUN(test_checkpoint_roundtrip);
    T_RUN(test_abort_then_checkpoint);
    T_RUN(test_max_page_id_offset);
    T_RUN(test_journal_enospc_nonfatal);
    T_RUN(test_fsync_failure_fatal);
    T_RUN(test_recovery_table);
    T_RUN(test_journal_creation_sync_retry);
    T_RUN(test_crash_every_point_checkpoint);
    T_RUN(test_crash_every_point_format);
    T_RUN(test_read_concurrent_with_checkpoint);
    return t_done();
}
