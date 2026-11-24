#include "fault_vfs.h"
#include "rng.h"
#include "test.h"

#include "pagestore/vfs.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static fault_vfs *fv;
static const ps_vfs *v;

static void setup(u64 seed)
{
    fv = fault_vfs_create(seed);
    v = fault_vfs_get(fv);
    fault_vfs_mkdir_durable(fv, "/d");
}

static void teardown(void)
{
    fault_vfs_destroy(fv);
    fv = NULL;
    v = NULL;
}

static ps_file open_file(const char *path, u32 flags)
{
    ps_file f;
    T_OK(v->open(v->ctx, path, flags, &f));
    return f;
}

static u64 fsize(ps_file *f)
{
    u64 sz = 0;
    T_OK(v->size(v->ctx, f, &sz));
    return sz;
}

static bool all_byte(const u8 *b, usize n, u8 c)
{
    for (usize i = 0; i < n; i++) {
        if (b[i] != c) {
            return false;
        }
    }
    return true;
}

/* Sync both the parent entry and the file to build a crash-safe fixture. */
static ps_file durable_file(const char *path, usize len, u8 fill)
{
    ps_file f = open_file(path, PS_VFS_CREATE);
    T_OK(v->fsync_dir(v->ctx, "/d"));
    u8 buf[4096];
    T_CHECK(len <= sizeof(buf));
    memset(buf, fill, len);
    if (len > 0) {
        T_OK(v->pwrite(v->ctx, &f, buf, len, 0));
    }
    T_OK(v->fsync(v->ctx, &f));
    return f;
}

static bool exists(const char *path)
{
    ps_file f;
    const int rv = v->open(v->ctx, path, 0, &f);
    if (rv == ENOENT) {
        return false;
    }
    T_OK(rv);
    v->close(v->ctx, &f);
    return true;
}

static void test_rng(void)
{
    rng a;
    rng b;
    rng_seed(&a, 42);
    rng_seed(&b, 42);
    for (u32 i = 0; i < 100; i++) {
        T_EQ(rng_next(&a), rng_next(&b));
    }
    rng_seed(&b, 43);
    T_NE(rng_next(&a), rng_next(&b));

    u32 hits[5] = {0};
    for (u32 i = 0; i < 5000; i++) {
        const u64 x = rng_below(&a, 5);
        T_CHECK(x < 5);
        hits[x]++;
    }
    for (u32 i = 0; i < 5; i++) {
        T_CHECK(hits[i] > 800);
    }
    T_EQ(rng_below(&a, 0), 0);
    T_EQ(rng_below(&a, 1), 0);
}

static void test_basic_io(void)
{
    setup(1);
    T_CHECK(v->open != NULL && v->close != NULL && v->pread != NULL && v->pwrite != NULL);
    T_CHECK(v->fsync != NULL && v->size != NULL && v->truncate != NULL && v->preallocate != NULL);
    T_CHECK(v->unlink != NULL && v->rename != NULL && v->mkdir != NULL && v->fsync_dir != NULL);
    T_CHECK(v->list != NULL && v->lock != NULL);

    ps_file f;
    T_ERR(v->open(v->ctx, "/d/f", 0, &f), ENOENT);
    T_ERR(v->open(v->ctx, "/nodir/f", PS_VFS_CREATE, &f), ENOENT);
    T_ERR(v->open(v->ctx, "/d", 0, &f), EISDIR);
    f = open_file("/d/f", PS_VFS_CREATE);
    ps_file g;
    T_ERR(v->open(v->ctx, "/d/f", PS_VFS_CREATE | PS_VFS_EXCL, &g), EEXIST);
    T_ERR(v->open(v->ctx, "/d/f/x", PS_VFS_CREATE, &g), ENOTDIR);
    T_EQ(fsize(&f), 0);

    T_OK(v->pwrite(v->ctx, &f, "0123456789", 10, 4090));
    T_EQ(fsize(&f), 4100);
    u8 buf[8192];
    usize got = 0;
    memset(buf, 0xAA, sizeof(buf));
    T_OK(v->pread(v->ctx, &f, buf, sizeof(buf), 0, &got));
    T_EQ(got, 4100);
    T_CHECK(all_byte(buf, 4090, 0));
    T_MEMEQ(buf + 4090, "0123456789", 10);
    T_OK(v->pread(v->ctx, &f, buf, 10, 5000, &got));
    T_EQ(got, 0);

    g = open_file("/d/f", 0);
    T_OK(v->pread(v->ctx, &g, buf, 4, 4094, &got));
    T_EQ(got, 4);
    T_MEMEQ(buf, "4567", 4);

    /* Cut inside a block to catch stale bytes after regrowth. */
    T_OK(v->truncate(v->ctx, &f, 4095));
    T_EQ(fsize(&g), 4095);
    T_OK(v->truncate(v->ctx, &f, 4100));
    T_OK(v->pread(v->ctx, &g, buf, 10, 4090, &got));
    T_EQ(got, 10);
    T_MEMEQ(buf, "01234\0\0\0\0\0", 10);

    T_OK(v->preallocate(v->ctx, &f, 100));
    T_EQ(fsize(&f), 4100);
    T_OK(v->preallocate(v->ctx, &f, 10000));
    T_EQ(fsize(&f), 10000);

    T_ERR(v->mkdir(v->ctx, "/d"), EEXIST);
    T_OK(v->mkdir(v->ctx, "/d/sub"));
    T_ERR(v->unlink(v->ctx, "/d/sub"), EISDIR);
    T_ERR(v->unlink(v->ctx, "/d/none"), ENOENT);
    T_ERR(v->fsync_dir(v->ctx, "/d/none"), ENOENT);
    T_ERR(v->fsync_dir(v->ctx, "/d/f"), ENOTDIR);
    T_ERR(v->rename(v->ctx, "/d/f", "/d/sub/f"), EXDEV);
    T_ERR(v->rename(v->ctx, "/d/none", "/d/x"), ENOENT);

    T_OK(v->unlink(v->ctx, "/d/f"));
    T_CHECK(!exists("/d/f"));
    T_OK(v->pwrite(v->ctx, &g, "z", 1, 0));
    T_OK(v->pread(v->ctx, &f, buf, 1, 0, &got));
    T_EQ(got, 1);
    T_EQ(buf[0], 'z');
    v->close(v->ctx, &f);
    v->close(v->ctx, &g);
    teardown();
}

/* list() collector: up to 8 names, sorted. */
struct ps_vfs_list_ctx {
    u32 n;
    char name[8][16];
};

static int collect(ps_vfs_list_ctx *lc, const char *name)
{
    T_CHECK(lc->n < 8);
    T_CHECK(strlen(name) < sizeof(lc->name[0]));
    u32 i = lc->n++;
    strcpy(lc->name[i], name);
    while (i > 0 && strcmp(lc->name[i - 1], lc->name[i]) > 0) {
        char tmp[16];
        strcpy(tmp, lc->name[i - 1]);
        strcpy(lc->name[i - 1], lc->name[i]);
        strcpy(lc->name[i], tmp);
        i--;
    }
    /* Reenter the VFS to catch a callback invoked under its mutex. */
    ps_file f;
    T_ERR(v->open(v->ctx, "/d/missing", 0, &f), ENOENT);
    return strcmp(name, "stop") == 0 ? 7 : 0;
}

static void test_list_lock(void)
{
    setup(1);
    ps_file a = open_file("/d/a", PS_VFS_CREATE);
    ps_file b = open_file("/d/b", PS_VFS_CREATE);
    T_OK(v->mkdir(v->ctx, "/d/c"));
    ps_vfs_list_ctx lc;
    memset(&lc, 0, sizeof(lc));
    T_OK(v->list(v->ctx, "/d", collect, &lc));
    T_EQ(lc.n, 3);
    T_CHECK(strcmp(lc.name[0], "a") == 0);
    T_CHECK(strcmp(lc.name[1], "b") == 0);
    T_CHECK(strcmp(lc.name[2], "c") == 0);
    T_ERR(v->list(v->ctx, "/d/none", collect, &lc), ENOENT);
    T_ERR(v->list(v->ctx, "/d/a", collect, &lc), ENOTDIR);

    ps_file s = open_file("/d/c/stop", PS_VFS_CREATE);
    memset(&lc, 0, sizeof(lc));
    T_ERR(v->list(v->ctx, "/d/c", collect, &lc), 7);
    T_EQ(lc.n, 1);

    ps_file l1;
    ps_file l2;
    T_OK(v->lock(v->ctx, "/d/LOCK", &l1));
    T_CHECK(exists("/d/LOCK"));
    T_ERR(v->lock(v->ctx, "/d/LOCK", &l2), EBUSY);
    v->close(v->ctx, &l1);
    T_OK(v->lock(v->ctx, "/d/LOCK", &l2));
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    T_OK(v->lock(v->ctx, "/d/LOCK", &l1));
    v->close(v->ctx, &l1);
    v->close(v->ctx, &l2);
    v->close(v->ctx, &a);
    v->close(v->ctx, &b);
    v->close(v->ctx, &s);
    teardown();
}

static void test_unsynced_create_lost(void)
{
    setup(1);
    ps_file f = open_file("/d/f", PS_VFS_CREATE);
    T_OK(v->pwrite(v->ctx, &f, "abc", 3, 0));
    T_OK(v->fsync(v->ctx, &f)); /* Leave the parent directory unsynced. */
    v->close(v->ctx, &f);
    T_OK(v->mkdir(v->ctx, "/d/sub"));
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    T_ERR(v->open(v->ctx, "/d/f", 0, &f), ENOENT);
    T_ERR(v->fsync_dir(v->ctx, "/d/sub"), ENOENT);
    teardown();
}

static void test_fsync_dir_keeps_entry(void)
{
    setup(1);
    ps_file f = open_file("/d/f", PS_VFS_CREATE);
    T_OK(v->fsync_dir(v->ctx, "/d"));
    T_OK(v->pwrite(v->ctx, &f, "abc", 3, 0));
    v->close(v->ctx, &f);
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    f = open_file("/d/f", 0);
    T_EQ(fsize(&f), 0);
    v->close(v->ctx, &f);
    teardown();
}

static void test_fsync_keeps_data(void)
{
    setup(1);
    ps_file f = open_file("/d/f", PS_VFS_CREATE);
    T_OK(v->fsync_dir(v->ctx, "/d"));
    T_OK(v->pwrite(v->ctx, &f, "abc", 3, 0));
    T_OK(v->fsync(v->ctx, &f));
    T_OK(v->pwrite(v->ctx, &f, "xyz", 3, 1));
    v->close(v->ctx, &f);
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    f = open_file("/d/f", 0);
    T_EQ(fsize(&f), 3);
    u8 buf[8];
    usize got = 0;
    T_OK(v->pread(v->ctx, &f, buf, sizeof(buf), 0, &got));
    T_EQ(got, 3);
    T_MEMEQ(buf, "abc", 3);

    T_OK(v->truncate(v->ctx, &f, 1));
    v->close(v->ctx, &f);
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    f = open_file("/d/f", 0);
    T_EQ(fsize(&f), 3);
    T_OK(v->truncate(v->ctx, &f, 1));
    T_OK(v->fsync(v->ctx, &f));
    v->close(v->ctx, &f);
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    f = open_file("/d/f", 0);
    T_EQ(fsize(&f), 1);
    v->close(v->ctx, &f);
    teardown();
}

static void test_rename_needs_fsync_dir(void)
{
    setup(1);
    ps_file f = durable_file("/d/a", 3, 'a');
    v->close(v->ctx, &f);
    T_OK(v->rename(v->ctx, "/d/a", "/d/b"));
    T_CHECK(!exists("/d/a"));
    T_CHECK(exists("/d/b"));
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    T_CHECK(exists("/d/a"));
    T_CHECK(!exists("/d/b"));

    T_OK(v->rename(v->ctx, "/d/a", "/d/b"));
    T_OK(v->fsync_dir(v->ctx, "/d"));
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    T_CHECK(!exists("/d/a"));
    f = open_file("/d/b", 0);
    u8 buf[4];
    usize got = 0;
    T_OK(v->pread(v->ctx, &f, buf, sizeof(buf), 0, &got));
    T_EQ(got, 3);
    T_MEMEQ(buf, "aaa", 3);
    v->close(v->ctx, &f);

    f = durable_file("/d/c", 2, 'c');
    v->close(v->ctx, &f);
    T_OK(v->rename(v->ctx, "/d/b", "/d/c"));
    f = open_file("/d/c", 0);
    T_EQ(fsize(&f), 3);
    v->close(v->ctx, &f);
    teardown();
}

static void test_unlink_needs_fsync_dir(void)
{
    setup(1);
    ps_file f = durable_file("/d/a", 3, 'a');
    v->close(v->ctx, &f);
    T_OK(v->unlink(v->ctx, "/d/a"));
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    T_CHECK(exists("/d/a"));
    T_OK(v->unlink(v->ctx, "/d/a"));
    T_OK(v->fsync_dir(v->ctx, "/d"));
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    T_CHECK(!exists("/d/a"));
    teardown();
}

static void test_crash_at_freezes(void)
{
    setup(1);
    ps_file f = durable_file("/d/f", 0, 0);
    const u64 p0 = fault_vfs_points(fv);
    fault_vfs_crash_at(fv, p0 + 2);
    T_OK(v->pwrite(v->ctx, &f, "a", 1, 0));
    T_EQ(fault_vfs_points(fv), p0 + 1);
    T_CHECK(!fault_vfs_crashed(fv));
    T_ERR(v->pwrite(v->ctx, &f, "b", 1, 1), EIO);
    T_ERR(v->fsync(v->ctx, &f), EIO);
    T_CHECK(fault_vfs_crashed(fv));
    u8 buf[4];
    usize got = 0;
    u64 sz = 0;
    ps_file g;
    T_ERR(v->pread(v->ctx, &f, buf, 1, 0, &got), EIO);
    T_ERR(v->size(v->ctx, &f, &sz), EIO);
    T_ERR(v->open(v->ctx, "/d/f", 0, &g), EIO);
    T_ERR(v->mkdir(v->ctx, "/d/x"), EIO);
    T_EQ(fault_vfs_points(fv), p0 + 2);
    v->close(v->ctx, &f);

    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    T_CHECK(!fault_vfs_crashed(fv));
    f = open_file("/d/f", 0);
    T_EQ(fsize(&f), 0);
    v->close(v->ctx, &f);
    teardown();
}

static void test_points_counting(void)
{
    setup(1);
    u64 p = fault_vfs_points(fv);
    ps_file f;
    T_ERR(v->open(v->ctx, "/d/f", 0, &f), ENOENT);
    T_EQ(fault_vfs_points(fv), p);
    f = open_file("/d/f", PS_VFS_CREATE);
    T_EQ(fault_vfs_points(fv), ++p);
    ps_file g = open_file("/d/f", PS_VFS_CREATE);
    T_EQ(fault_vfs_points(fv), p);
    u8 buf[4];
    usize got = 0;
    u64 sz = 0;
    T_OK(v->pread(v->ctx, &f, buf, 1, 0, &got));
    T_OK(v->size(v->ctx, &f, &sz));
    T_EQ(fault_vfs_points(fv), p);
    T_OK(v->pwrite(v->ctx, &f, "a", 1, 0));
    T_OK(v->fsync(v->ctx, &f));
    T_OK(v->truncate(v->ctx, &f, 0));
    T_OK(v->preallocate(v->ctx, &f, 10));
    T_OK(v->mkdir(v->ctx, "/d/x"));
    T_OK(v->rename(v->ctx, "/d/f", "/d/g"));
    T_OK(v->unlink(v->ctx, "/d/g"));
    T_OK(v->fsync_dir(v->ctx, "/d"));
    T_EQ(fault_vfs_points(fv), p + 8);
    v->close(v->ctx, &f);
    v->close(v->ctx, &g);
    teardown();
}

static void test_stale_handle(void)
{
    setup(1);
    ps_file f = durable_file("/d/f", 3, 'a');
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    u8 buf[4];
    usize got = 0;
    T_ERR(v->pread(v->ctx, &f, buf, 1, 0, &got), EBADF);
    T_ERR(v->pwrite(v->ctx, &f, "x", 1, 0), EBADF);
    T_ERR(v->fsync(v->ctx, &f), EBADF);
    v->close(v->ctx, &f);
    teardown();
}

static void test_random_tears_on_512(void)
{
    u32 full = 0;
    u32 none = 0;
    u32 torn = 0;
    for (u64 seed = 1; seed <= 200; seed++) {
        setup(seed);
        ps_file f = durable_file("/d/f", 4096, 0);
        u8 buf[4096];
        memset(buf, 0xAB, sizeof(buf));
        T_OK(v->pwrite(v->ctx, &f, buf, sizeof(buf), 0));
        v->close(v->ctx, &f);
        fault_vfs_reboot(fv, FAULT_RANDOM_UNSYNCED);

        f = open_file("/d/f", 0);
        T_EQ(fsize(&f), 4096);
        usize got = 0;
        T_OK(v->pread(v->ctx, &f, buf, sizeof(buf), 0, &got));
        T_EQ(got, 4096);
        usize k = 0;
        while (k < sizeof(buf) && buf[k] == 0xAB) {
            k++;
        }
        T_EQ(k % 512, 0);
        T_CHECK(all_byte(buf + k, sizeof(buf) - k, 0));
        if (k == 4096) {
            full++;
        } else if (k == 0) {
            none++;
        } else {
            torn++;
        }
        v->close(v->ctx, &f);
        teardown();
    }
    T_CHECK(full > 0);
    T_CHECK(none > 0);
    T_CHECK(torn > 0);
}

/* Each unsynced directory operation replays with a 50% chance, so any subset can survive. */
static void test_random_dir_ops(void)
{
    u32 seen[4] = {0}; /* bit 0: a exists, bit 1: b exists */
    for (u64 seed = 1; seed <= 100; seed++) {
        setup(seed);
        ps_file f = open_file("/d/a", PS_VFS_CREATE);
        T_OK(v->fsync(v->ctx, &f));
        v->close(v->ctx, &f);
        f = open_file("/d/b", PS_VFS_CREATE);
        v->close(v->ctx, &f);
        fault_vfs_reboot(fv, FAULT_RANDOM_UNSYNCED);
        seen[(exists("/d/a") ? 1 : 0) | (exists("/d/b") ? 2 : 0)]++;
        teardown();
    }
    for (u32 i = 0; i < 4; i++) {
        T_CHECK(seen[i] > 0);
    }
}

/* The same seed gives the same post-crash state. */
static void test_random_deterministic(void)
{
    u64 sizes[2];
    for (u32 run = 0; run < 2; run++) {
        setup(7);
        ps_file f = durable_file("/d/f", 0, 0);
        u8 buf[1024];
        memset(buf, 1, sizeof(buf));
        for (u64 i = 0; i < 32; i++) {
            T_OK(v->pwrite(v->ctx, &f, buf, sizeof(buf), i * sizeof(buf)));
        }
        v->close(v->ctx, &f);
        fault_vfs_reboot(fv, FAULT_RANDOM_UNSYNCED);
        f = open_file("/d/f", 0);
        sizes[run] = fsize(&f);
        v->close(v->ctx, &f);
        teardown();
    }
    T_EQ(sizes[0], sizes[1]);
}

static void test_fail_at_once(void)
{
    setup(1);
    ps_file f = durable_file("/d/f", 0, 0);
    fault_vfs_fail_at(fv, fault_vfs_points(fv) + 1, ENOSPC);
    T_ERR(v->pwrite(v->ctx, &f, "abc", 3, 0), ENOSPC);
    T_EQ(fsize(&f), 0);
    T_CHECK(!fault_vfs_crashed(fv));
    T_OK(v->pwrite(v->ctx, &f, "abc", 3, 0));
    T_EQ(fsize(&f), 3);

    fault_vfs_fail_at(fv, fault_vfs_points(fv) + 1, EIO);
    T_ERR(v->unlink(v->ctx, "/d/f"), EIO);
    T_CHECK(exists("/d/f"));
    v->close(v->ctx, &f);
    teardown();
}

static void test_enospc_limit(void)
{
    setup(1);
    ps_file f = durable_file("/d/f", 0, 0);
    fault_vfs_enospc_after(fv, 8192);
    T_OK(v->preallocate(v->ctx, &f, 4096));
    T_ERR(v->preallocate(v->ctx, &f, 16384), ENOSPC);
    T_EQ(fsize(&f), 4096);
    T_ERR(v->pwrite(v->ctx, &f, "x", 1, 8192), ENOSPC);
    T_EQ(fsize(&f), 4096);
    T_OK(v->pwrite(v->ctx, &f, "x", 1, 100));
    ps_file g = open_file("/d/g", PS_VFS_CREATE);
    T_OK(v->truncate(v->ctx, &g, 4096));
    T_ERR(v->truncate(v->ctx, &g, 4097), ENOSPC);
    v->close(v->ctx, &g);
    T_OK(v->unlink(v->ctx, "/d/g"));
    T_OK(v->preallocate(v->ctx, &f, 8192));
    fault_vfs_enospc_after(fv, UINT64_MAX);
    T_OK(v->preallocate(v->ctx, &f, 1u << 20));
    v->close(v->ctx, &f);
    teardown();
}

static void test_sparse_high_offset(void)
{
    setup(1);
    ps_file f = durable_file("/d/f", 0, 0);
    const u64 off = 0xFFFFFFFFull * 4096;
    T_OK(v->pwrite(v->ctx, &f, "q", 1, off));
    T_EQ(fsize(&f), off + 1);
    u8 buf[4096];
    usize got = 0;
    memset(buf, 0xEE, sizeof(buf));
    T_OK(v->pread(v->ctx, &f, buf, sizeof(buf), 0, &got));
    T_EQ(got, sizeof(buf));
    T_CHECK(all_byte(buf, sizeof(buf), 0));
    T_OK(v->pread(v->ctx, &f, buf, 2, off - 1, &got));
    T_EQ(got, 2);
    T_EQ(buf[0], 0);
    T_EQ(buf[1], 'q');
    T_OK(v->fsync(v->ctx, &f));
    v->close(v->ctx, &f);
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    f = open_file("/d/f", 0);
    T_EQ(fsize(&f), off + 1);
    v->close(v->ctx, &f);
    teardown();
}

static void test_corrupt(void)
{
    setup(1);
    ps_file f = durable_file("/d/f", 4, 'a');
    T_ERR(fault_vfs_corrupt(fv, "/d/none", 0, 1), ENOENT);
    T_ERR(fault_vfs_corrupt(fv, "/d", 0, 1), EISDIR);
    T_ERR(fault_vfs_corrupt(fv, "/d/f", 4, 1), EINVAL);
    T_OK(fault_vfs_corrupt(fv, "/d/f", 2, 0x20));
    u8 buf[4];
    usize got = 0;
    T_OK(v->pread(v->ctx, &f, buf, 4, 0, &got));
    T_MEMEQ(buf, "aaAa", 4);
    v->close(v->ctx, &f);
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    f = open_file("/d/f", 0);
    T_OK(v->pread(v->ctx, &f, buf, 4, 0, &got));
    T_MEMEQ(buf, "aaAa", 4);
    v->close(v->ctx, &f);
    teardown();
}

static void test_mkdir_durable_nested(void)
{
    setup(1);
    fault_vfs_mkdir_durable(fv, "/d/e");
    fault_vfs_mkdir_durable(fv, "/d/e");
    ps_file f = open_file("/d/e/f", PS_VFS_CREATE);
    T_OK(v->fsync_dir(v->ctx, "/d/e"));
    v->close(v->ctx, &f);
    fault_vfs_reboot(fv, FAULT_DROP_UNSYNCED);
    T_CHECK(exists("/d/e/f"));
    teardown();
}

/* Concurrent calls are serialized (checked under TSan). */
struct worker {
    pthread_t th;
    char path[16];
    u8 fill;
};

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    ps_file f;
    T_OK(v->open(v->ctx, w->path, PS_VFS_CREATE, &f));
    u8 buf[700];
    memset(buf, w->fill, sizeof(buf));
    for (u64 i = 0; i < 200; i++) {
        T_OK(v->pwrite(v->ctx, &f, buf, sizeof(buf), i * sizeof(buf)));
        if (i % 16 == 0) {
            T_OK(v->fsync(v->ctx, &f));
        }
    }
    usize got = 0;
    T_OK(v->pread(v->ctx, &f, buf, sizeof(buf), 199 * sizeof(buf), &got));
    T_EQ(got, sizeof(buf));
    T_CHECK(all_byte(buf, sizeof(buf), w->fill));
    v->close(v->ctx, &f);
    return NULL;
}

static void test_threads(void)
{
    setup(1);
    struct worker w[4];
    for (u32 i = 0; i < 4; i++) {
        snprintf(w[i].path, sizeof(w[i].path), "/d/t%u", (unsigned) i);
        w[i].fill = (u8) (i + 1);
        T_EQ(pthread_create(&w[i].th, NULL, worker_main, &w[i]), 0);
    }
    for (u32 i = 0; i < 4; i++) {
        T_EQ(pthread_join(w[i].th, NULL), 0);
    }
    /* Each thread creates once, writes 200 times and syncs at 0, 16, ..., 192. */
    T_EQ(fault_vfs_points(fv), 4 * (1 + 200 + 13));
    teardown();
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_rng);
    T_RUN(test_basic_io);
    T_RUN(test_list_lock);
    T_RUN(test_unsynced_create_lost);
    T_RUN(test_fsync_dir_keeps_entry);
    T_RUN(test_fsync_keeps_data);
    T_RUN(test_rename_needs_fsync_dir);
    T_RUN(test_unlink_needs_fsync_dir);
    T_RUN(test_crash_at_freezes);
    T_RUN(test_points_counting);
    T_RUN(test_stale_handle);
    T_RUN(test_random_tears_on_512);
    T_RUN(test_random_dir_ops);
    T_RUN(test_random_deterministic);
    T_RUN(test_fail_at_once);
    T_RUN(test_enospc_limit);
    T_RUN(test_sparse_high_offset);
    T_RUN(test_corrupt);
    T_RUN(test_mkdir_durable_nested);
    T_RUN(test_threads);
    return t_done();
}
