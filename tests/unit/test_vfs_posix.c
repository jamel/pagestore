#include "pagestore/vfs.h"
#include "test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const ps_vfs *v;
static char *dir;

static char *path(const char *name)
{
    size_t n = strlen(dir) + 1 + strlen(name) + 1;
    char *p = malloc(n);
    T_CHECK(p != NULL);
    snprintf(p, n, "%s/%s", dir, name);
    return p;
}

static void setup(void)
{
    v = ps_vfs_posix();
    T_CHECK(v != NULL);
    dir = t_tmpdir();
}

static void teardown(void)
{
    t_rmtree(dir);
    free(dir);
    dir = NULL;
}

static ps_file *create(const char *name)
{
    char *p = path(name);
    ps_file *f = NULL;
    T_OK(v->open(v->ctx, p, PS_VFS_CREATE, &f));
    T_CHECK(f != NULL);
    free(p);
    return f;
}

static uint64_t fsize(ps_file *f)
{
    uint64_t sz = 0;
    T_OK(v->size(v->ctx, f, &sz));
    return sz;
}

static int all_zero(const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (b[i] != 0)
            return 0;
    return 1;
}

/* list() collector: names joined into a sorted, fixed-size table. */
struct names {
    int n;
    char name[8][64];
};

static int collect(void *arg, const char *name)
{
    struct names *ns = arg;
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
    struct names ns;
    memset(&ns, 0, sizeof(ns));
    T_OK(v->list(v->ctx, d, collect, &ns));
    return ns;
}

static void test_create_write_read(void)
{
    setup();
    ps_file *f = create("f");
    T_EQ(fsize(f), 0);

    T_OK(v->pwrite(v->ctx, f, "0123456789", 10, 4096));
    T_EQ(fsize(f), 4106);

    uint8_t buf[8192];
    size_t got = 1234;
    memset(buf, 0xAA, sizeof(buf));
    T_OK(v->pread(v->ctx, f, buf, 4106, 0, &got));
    T_EQ(got, 4106);
    T_CHECK(all_zero(buf, 4096));
    T_MEMEQ(buf + 4096, "0123456789", 10);

    T_OK(v->pread(v->ctx, f, buf, 100, 4100, &got));
    T_EQ(got, 6);
    T_MEMEQ(buf, "456789", 6);

    got = 1234;
    T_OK(v->pread(v->ctx, f, buf, 10, 5000, &got));
    T_EQ(got, 0);

    T_OK(v->pwrite(v->ctx, f, "ab", 2, 4097));
    v->close(v->ctx, f);
    char *p = path("f");
    f = NULL;
    T_OK(v->open(v->ctx, p, 0, &f));
    T_OK(v->pread(v->ctx, f, buf, 10, 4096, &got));
    T_EQ(got, 10);
    T_MEMEQ(buf, "0ab3456789", 10);
    v->close(v->ctx, f);
    T_OK(v->open(v->ctx, p, PS_VFS_CREATE, &f));
    T_EQ(fsize(f), 4106);
    v->close(v->ctx, f);
    free(p);
    teardown();
}

static void test_open_errors(void)
{
    setup();
    char *missing = path("missing");
    char *existing = path("existing");
    ps_file *f = NULL;

    T_ERR(v->open(v->ctx, missing, 0, &f), ENOENT);
    T_ERR(v->open(v->ctx, missing, PS_VFS_EXCL, &f), ENOENT);

    T_OK(v->open(v->ctx, existing, PS_VFS_CREATE | PS_VFS_EXCL, &f));
    v->close(v->ctx, f);
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
    ps_file *f = create("f");
    T_OK(v->preallocate(v->ctx, f, 1u << 20));
    T_EQ(fsize(f), 1u << 20);

    static uint8_t buf[1u << 20];
    size_t got = 0;
    memset(buf, 0xAA, sizeof(buf));
    T_OK(v->pread(v->ctx, f, buf, sizeof(buf), 0, &got));
    T_EQ(got, sizeof(buf));
    T_CHECK(all_zero(buf, sizeof(buf)));

    T_OK(v->pwrite(v->ctx, f, "x", 1, (1u << 20) + 99));
    T_OK(v->preallocate(v->ctx, f, 4096));
    T_EQ(fsize(f), (1u << 20) + 100);
    T_OK(v->preallocate(v->ctx, f, 0));
    T_EQ(fsize(f), (1u << 20) + 100);

    ps_file *g = create("g");
    T_OK(v->pwrite(v->ctx, g, "hello", 5, 0));
    T_OK(v->preallocate(v->ctx, g, 65536));
    T_EQ(fsize(g), 65536);
    T_OK(v->pread(v->ctx, g, buf, 65536, 0, &got));
    T_EQ(got, 65536);
    T_MEMEQ(buf, "hello", 5);
    T_CHECK(all_zero(buf + 5, 65536 - 5));

    v->close(v->ctx, g);
    v->close(v->ctx, f);
    teardown();
}

static void test_truncate(void)
{
    setup();
    ps_file *f = create("f");
    uint8_t buf[200];
    size_t got = 0;

    memset(buf, 'z', sizeof(buf));
    T_OK(v->pwrite(v->ctx, f, buf, sizeof(buf), 0));
    T_OK(v->truncate(v->ctx, f, 100));
    T_EQ(fsize(f), 100);
    T_OK(v->pread(v->ctx, f, buf, sizeof(buf), 0, &got));
    T_EQ(got, 100);

    T_OK(v->truncate(v->ctx, f, 150));
    T_EQ(fsize(f), 150);
    T_OK(v->pread(v->ctx, f, buf, sizeof(buf), 0, &got));
    T_EQ(got, 150);
    T_EQ(buf[99], 'z');
    T_CHECK(all_zero(buf + 100, 50));

    T_OK(v->truncate(v->ctx, f, 0));
    T_EQ(fsize(f), 0);
    v->close(v->ctx, f);
    teardown();
}

static void test_rename_unlink_list(void)
{
    setup();
    v->close(v->ctx, create("a"));
    v->close(v->ctx, create("b"));

    char *a = path("a"), *b = path("b"), *c = path("c");
    T_OK(v->rename(v->ctx, a, c));

    struct names ns = list_dir(dir);
    T_EQ(ns.n, 2);
    T_CHECK(strcmp(ns.name[0], "b") == 0);
    T_CHECK(strcmp(ns.name[1], "c") == 0);

    T_OK(v->unlink(v->ctx, b));
    ns = list_dir(dir);
    T_EQ(ns.n, 1);
    T_CHECK(strcmp(ns.name[0], "c") == 0);

    T_ERR(v->unlink(v->ctx, b), ENOENT);
    T_ERR(v->rename(v->ctx, a, b), ENOENT);

    ps_file *f = create("a");
    T_OK(v->pwrite(v->ctx, f, "new", 3, 0));
    v->close(v->ctx, f);
    f = NULL;
    T_OK(v->open(v->ctx, c, 0, &f));
    T_OK(v->pwrite(v->ctx, f, "old!", 4, 0));
    v->close(v->ctx, f);
    T_OK(v->rename(v->ctx, a, c));
    T_OK(v->open(v->ctx, c, 0, &f));
    T_EQ(fsize(f), 3);
    char buf[8];
    size_t got = 0;
    T_OK(v->pread(v->ctx, f, buf, sizeof(buf), 0, &got));
    T_EQ(got, 3);
    T_MEMEQ(buf, "new", 3);
    v->close(v->ctx, f);
    ns = list_dir(dir);
    T_EQ(ns.n, 1);
    T_CHECK(strcmp(ns.name[0], "c") == 0);

    char *sub = path("sub");
    T_OK(v->mkdir(v->ctx, sub));
    ns = list_dir(sub);
    T_EQ(ns.n, 0);

    char *nodir = path("nodir");
    T_ERR(v->list(v->ctx, nodir, collect, &ns), ENOENT);

    free(nodir);
    free(sub);
    free(a);
    free(b);
    free(c);
    teardown();
}

static int stop_after_first(void *arg, const char *name)
{
    (void)name;
    int *calls = arg;
    (*calls)++;
    return 77;
}

static void test_list_stop(void)
{
    setup();
    v->close(v->ctx, create("a"));
    v->close(v->ctx, create("b"));
    v->close(v->ctx, create("c"));
    int calls = 0;
    T_ERR(v->list(v->ctx, dir, stop_after_first, &calls), 77);
    T_EQ(calls, 1);
    teardown();
}

static void test_mkdir_eexist(void)
{
    setup();
    char *d = path("d");
    T_OK(v->mkdir(v->ctx, d));
    T_ERR(v->mkdir(v->ctx, d), EEXIST);

    char *f = path("d/f");
    ps_file *h = NULL;
    T_OK(v->open(v->ctx, f, PS_VFS_CREATE, &h));
    v->close(v->ctx, h);

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
    ps_file *l1 = NULL, *l2 = NULL;

    T_OK(v->lock(v->ctx, p, &l1));
    T_ERR(v->lock(v->ctx, p, &l2), EBUSY);
    T_ERR(v->lock(v->ctx, p, &l2), EBUSY);

    /* Closing a separately opened handle would release a process-owned lock. */
    ps_file *f = NULL;
    T_OK(v->open(v->ctx, p, 0, &f));
    v->close(v->ctx, f);
    T_ERR(v->lock(v->ctx, p, &l2), EBUSY);

    v->close(v->ctx, l1);
    T_OK(v->lock(v->ctx, p, &l2));
    T_ERR(v->lock(v->ctx, p, &l1), EBUSY);
    v->close(v->ctx, l2);

    char *nodir = path("nodir/LOCK");
    T_ERR(v->lock(v->ctx, nodir, &l1), ENOENT);

    free(nodir);
    free(p);
    teardown();
}

static void test_fsync_ok(void)
{
    setup();
    ps_file *f = create("f");
    T_OK(v->pwrite(v->ctx, f, "data", 4, 0));
    T_OK(v->fsync(v->ctx, f));
    T_OK(v->fsync_dir(v->ctx, dir));
    v->close(v->ctx, f);

    char *nodir = path("nodir");
    T_ERR(v->fsync_dir(v->ctx, nodir), ENOENT);
    free(nodir);
    teardown();
}

static void test_ctx_shared(void)
{
    /* The POSIX vfs is a singleton. */
    T_CHECK(ps_vfs_posix() == ps_vfs_posix());
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
    T_RUN(test_ctx_shared);
    return t_done();
}
