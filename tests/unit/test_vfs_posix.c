#include "test.h"

#include "pagestore/vfs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ps_vfs vfs;
static const ps_vfs *v = &vfs;
static char *dir;

static char *path(const char *name)
{
    usize n = strlen(dir) + 1 + strlen(name) + 1;
    char *p = t_malloc(n);
    snprintf(p, n, "%s/%s", dir, name);
    return p;
}

static void setup(void)
{
    ps_vfs_posix_init(&vfs);
    dir = t_tmpdir();
}

static void teardown(void)
{
    t_rmtree(dir);
    free(dir);
    dir = NULL;
}

static ps_file create(const char *name)
{
    char *p = path(name);
    ps_file f;
    T_OK(v->open(v->ctx, p, PS_VFS_CREATE, &f));
    free(p);
    return f;
}

static void close_file(ps_file f)
{
    v->close(v->ctx, &f);
}

static u64 fsize(ps_file *f)
{
    u64 sz = 0;
    T_OK(v->size(v->ctx, f, &sz));
    return sz;
}

static bool all_zero(const u8 *b, usize n)
{
    for (usize i = 0; i < n; i++) {
        if (b[i] != 0) {
            return false;
        }
    }
    return true;
}

/* list() collector: names joined into a sorted, fixed-size table. */
struct names {
    int n;
    char name[8][64];
};

/* The list() context of this file: collect() uses names, stop_after_first() uses calls. */
struct ps_vfs_list_ctx {
    struct names names;
    int calls;
};

static int collect(ps_vfs_list_ctx *arg, const char *name)
{
    struct names *ns = &arg->names;
    T_CHECK(ns->n < 8);
    T_CHECK(strlen(name) < sizeof(ns->name[0]));
    int i = ns->n++;
    strcpy(ns->name[i], name);
    while (i > 0 && strcmp(ns->name[i - 1], ns->name[i]) > 0) {
        char tmp[64];
        strcpy(tmp, ns->name[i - 1]);
        strcpy(ns->name[i - 1], ns->name[i]);
        strcpy(ns->name[i], tmp);
        i--;
    }
    return 0;
}

static struct names list_dir(const char *d)
{
    ps_vfs_list_ctx lc;
    memset(&lc, 0, sizeof(lc));
    T_OK(v->list(v->ctx, d, collect, &lc));
    return lc.names;
}

static void test_create_write_read(void)
{
    setup();
    ps_file f = create("f");
    T_EQ(fsize(&f), 0);

    T_OK(v->pwrite(v->ctx, &f, "0123456789", 10, 4096));
    T_EQ(fsize(&f), 4106);

    u8 buf[8192];
    usize got = 1234;
    memset(buf, 0xAA, sizeof(buf));
    T_OK(v->pread(v->ctx, &f, buf, 4106, 0, &got));
    T_EQ(got, 4106);
    T_CHECK(all_zero(buf, 4096));
    T_MEMEQ(buf + 4096, "0123456789", 10);

    T_OK(v->pread(v->ctx, &f, buf, 100, 4100, &got));
    T_EQ(got, 6);
    T_MEMEQ(buf, "456789", 6);

    got = 1234;
    T_OK(v->pread(v->ctx, &f, buf, 10, 5000, &got));
    T_EQ(got, 0);

    T_OK(v->pwrite(v->ctx, &f, "ab", 2, 4097));
    v->close(v->ctx, &f);
    char *p = path("f");
    T_OK(v->open(v->ctx, p, 0, &f));
    T_OK(v->pread(v->ctx, &f, buf, 10, 4096, &got));
    T_EQ(got, 10);
    T_MEMEQ(buf, "0ab3456789", 10);
    v->close(v->ctx, &f);
    T_OK(v->open(v->ctx, p, PS_VFS_CREATE, &f));
    T_EQ(fsize(&f), 4106);
    v->close(v->ctx, &f);
    free(p);
    teardown();
}

static void test_open_errors(void)
{
    setup();
    char *missing = path("missing");
    char *existing = path("existing");
    ps_file f;

    T_ERR(v->open(v->ctx, missing, 0, &f), ENOENT);
    T_ERR(v->open(v->ctx, missing, PS_VFS_EXCL, &f), ENOENT);

    T_OK(v->open(v->ctx, existing, PS_VFS_CREATE | PS_VFS_EXCL, &f));
    v->close(v->ctx, &f);
    T_ERR(v->open(v->ctx, existing, PS_VFS_CREATE | PS_VFS_EXCL, &f), EEXIST);

    char *nodir = path("nodir/x");
    T_ERR(v->open(v->ctx, nodir, PS_VFS_CREATE, &f), ENOENT);

    free(nodir);
    free(missing);
    free(existing);
    teardown();
}

static void test_preallocate(void)
{
    setup();
    ps_file f = create("f");
    T_OK(v->preallocate(v->ctx, &f, 1u << 20));
    T_EQ(fsize(&f), 1u << 20);

    static u8 buf[1u << 20];
    usize got = 0;
    memset(buf, 0xAA, sizeof(buf));
    T_OK(v->pread(v->ctx, &f, buf, sizeof(buf), 0, &got));
    T_EQ(got, sizeof(buf));
    T_CHECK(all_zero(buf, sizeof(buf)));

    T_OK(v->pwrite(v->ctx, &f, "x", 1, (1u << 20) + 99));
    T_OK(v->preallocate(v->ctx, &f, 4096));
    T_EQ(fsize(&f), (1u << 20) + 100);
    T_OK(v->preallocate(v->ctx, &f, 0));
    T_EQ(fsize(&f), (1u << 20) + 100);

    ps_file g = create("g");
    T_OK(v->pwrite(v->ctx, &g, "hello", 5, 0));
    T_OK(v->preallocate(v->ctx, &g, 65536));
    T_EQ(fsize(&g), 65536);
    T_OK(v->pread(v->ctx, &g, buf, 65536, 0, &got));
    T_EQ(got, 65536);
    T_MEMEQ(buf, "hello", 5);
    T_CHECK(all_zero(buf + 5, 65536 - 5));

    v->close(v->ctx, &g);
    v->close(v->ctx, &f);
    teardown();
}

static void test_truncate(void)
{
    setup();
    ps_file f = create("f");
    u8 buf[200];
    usize got = 0;

    memset(buf, 'z', sizeof(buf));
    T_OK(v->pwrite(v->ctx, &f, buf, sizeof(buf), 0));
    T_OK(v->truncate(v->ctx, &f, 100));
    T_EQ(fsize(&f), 100);
    T_OK(v->pread(v->ctx, &f, buf, sizeof(buf), 0, &got));
    T_EQ(got, 100);

    T_OK(v->truncate(v->ctx, &f, 150));
    T_EQ(fsize(&f), 150);
    T_OK(v->pread(v->ctx, &f, buf, sizeof(buf), 0, &got));
    T_EQ(got, 150);
    T_EQ(buf[99], 'z');
    T_CHECK(all_zero(buf + 100, 50));

    T_OK(v->truncate(v->ctx, &f, 0));
    T_EQ(fsize(&f), 0);
    v->close(v->ctx, &f);
    teardown();
}

static void test_rename_unlink_list(void)
{
    setup();
    close_file(create("a"));
    close_file(create("b"));

    char *a = path("a"), *b = path("b"), *c = path("c");
    T_OK(v->rename(v->ctx, a, c));

    struct names ns = list_dir(dir);
    T_EQ(ns.n, 2);
    T_STREQ(ns.name[0], "b");
    T_STREQ(ns.name[1], "c");

    T_OK(v->unlink(v->ctx, b));
    ns = list_dir(dir);
    T_EQ(ns.n, 1);
    T_STREQ(ns.name[0], "c");

    T_ERR(v->unlink(v->ctx, b), ENOENT);
    T_ERR(v->rename(v->ctx, a, b), ENOENT);

    ps_file f = create("a");
    T_OK(v->pwrite(v->ctx, &f, "new", 3, 0));
    v->close(v->ctx, &f);
    T_OK(v->open(v->ctx, c, 0, &f));
    T_OK(v->pwrite(v->ctx, &f, "old!", 4, 0));
    v->close(v->ctx, &f);
    T_OK(v->rename(v->ctx, a, c));
    T_OK(v->open(v->ctx, c, 0, &f));
    T_EQ(fsize(&f), 3);
    char buf[8];
    usize got = 0;
    T_OK(v->pread(v->ctx, &f, buf, sizeof(buf), 0, &got));
    T_EQ(got, 3);
    T_MEMEQ(buf, "new", 3);
    v->close(v->ctx, &f);
    ns = list_dir(dir);
    T_EQ(ns.n, 1);
    T_STREQ(ns.name[0], "c");

    char *sub = path("sub");
    T_OK(v->mkdir(v->ctx, sub));
    ns = list_dir(sub);
    T_EQ(ns.n, 0);

    char *nodir = path("nodir");
    ps_vfs_list_ctx lc;
    memset(&lc, 0, sizeof(lc));
    T_ERR(v->list(v->ctx, nodir, collect, &lc), ENOENT);

    free(nodir);
    free(sub);
    free(a);
    free(b);
    free(c);
    teardown();
}

static int stop_after_first(ps_vfs_list_ctx *arg, const char *name)
{
    PS_UNUSED(name);
    arg->calls++;
    return 77;
}

static void test_list_stop(void)
{
    setup();
    close_file(create("a"));
    close_file(create("b"));
    close_file(create("c"));
    ps_vfs_list_ctx lc;
    memset(&lc, 0, sizeof(lc));
    T_ERR(v->list(v->ctx, dir, stop_after_first, &lc), 77);
    T_EQ(lc.calls, 1);
    teardown();
}

static void test_mkdir_eexist(void)
{
    setup();
    char *d = path("d");
    T_OK(v->mkdir(v->ctx, d));
    T_ERR(v->mkdir(v->ctx, d), EEXIST);

    char *f = path("d/f");
    ps_file h;
    T_OK(v->open(v->ctx, f, PS_VFS_CREATE, &h));
    v->close(v->ctx, &h);

    char *deep = path("x/y");
    T_ERR(v->mkdir(v->ctx, deep), ENOENT);

    free(deep);
    free(f);
    free(d);
    teardown();
}

static void test_lock_conflict(void)
{
    setup();
    char *p = path("LOCK");
    ps_file l1, l2;

    T_OK(v->lock(v->ctx, p, &l1));
    T_ERR(v->lock(v->ctx, p, &l2), EBUSY);
    T_ERR(v->lock(v->ctx, p, &l2), EBUSY);

    /* Closing a separately opened handle would release a process-owned lock. */
    ps_file f;
    T_OK(v->open(v->ctx, p, 0, &f));
    v->close(v->ctx, &f);
    T_ERR(v->lock(v->ctx, p, &l2), EBUSY);

    v->close(v->ctx, &l1);
    T_OK(v->lock(v->ctx, p, &l2));
    T_ERR(v->lock(v->ctx, p, &l1), EBUSY);
    v->close(v->ctx, &l2);

    char *nodir = path("nodir/LOCK");
    T_ERR(v->lock(v->ctx, nodir, &l1), ENOENT);

    free(nodir);
    free(p);
    teardown();
}

static void test_fsync_ok(void)
{
    setup();
    ps_file f = create("f");
    T_OK(v->pwrite(v->ctx, &f, "data", 4, 0));
    T_OK(v->fsync(v->ctx, &f));
    T_OK(v->fsync_dir(v->ctx, dir));
    v->close(v->ctx, &f);

    char *nodir = path("nodir");
    T_ERR(v->fsync_dir(v->ctx, nodir), ENOENT);
    free(nodir);
    teardown();
}

static void test_init_fills_all(void)
{
    ps_vfs a;
    memset(&a, 0xAB, sizeof(a));
    ps_vfs_posix_init(&a);
    T_CHECK(a.ctx == NULL);
    T_CHECK(a.open != NULL && a.close != NULL && a.pread != NULL && a.pwrite != NULL);
    T_CHECK(a.fsync != NULL && a.size != NULL && a.truncate != NULL && a.preallocate != NULL);
    T_CHECK(a.unlink != NULL && a.rename != NULL && a.mkdir != NULL && a.fsync_dir != NULL);
    T_CHECK(a.list != NULL && a.lock != NULL);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_create_write_read);
    T_RUN(test_open_errors);
    T_RUN(test_preallocate);
    T_RUN(test_truncate);
    T_RUN(test_rename_unlink_list);
    T_RUN(test_list_stop);
    T_RUN(test_mkdir_eexist);
    T_RUN(test_lock_conflict);
    T_RUN(test_fsync_ok);
    T_RUN(test_init_fills_all);
    return t_done();
}
