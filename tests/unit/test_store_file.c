#include "backend_file.h"
#include "pagestore/pagestore.h"
#include "store.h"
#include "test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDR PS_PAGE_HEADER_SIZE

static char *root;
static char dir[4096];

static void setup(void)
{
    root = t_tmpdir();
    snprintf(dir, sizeof(dir), "%s/store", root);
}

static void teardown(void)
{
    t_rmtree(root);
    free(root);
    root = NULL;
}

static ps_config file_config(u32 page_size)
{
    ps_config cfg;
    ps_config_init(&cfg);
    cfg.page_size = page_size;
    cfg.cache_size = (usize) 1 << 20;
    cfg.checkpoint_thread = false;
    return cfg;
}

static bool exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

static void put_text(ps_page *p, const char *text)
{
    memcpy((u8 *) p + HDR, text, strlen(text) + 1);
}

static void check_text(ps_store *s, ps_page_id id, const char *text)
{
    ps_snapshot *snap = NULL;
    T_OK(ps_snapshot_open(s, &snap));
    const ps_page *p = NULL;
    T_OK(ps_snapshot_read(snap, id, &p));
    T_EQ(p->id.raw, id.raw);
    T_CHECK(strcmp((const char *) p + HDR, text) == 0);
    ps_snapshot_close(snap);
}

static void test_create_reopen(void)
{
    setup();
    const ps_config cfg = file_config(0);
    ps_store *s = NULL;
    T_ERR(ps_open(dir, &cfg, 0, &s), ENOENT);
    T_CHECK(!exists(dir));
    T_OK(ps_open(dir, &cfg, PS_CREATE, &s));
    T_EQ(ps_page_size(s), 4096);
    T_CHECK(strcmp(ps_store_dir(s), dir) == 0);
    T_CHECK(ps_store_vfs(s) != NULL);

    ps_page_id a = PS_PAGE_ID_NONE;
    ps_page_id b = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_op_begin(s, 10));
    T_OK(ps_alloc(s, &a, &p));
    put_text(p, "alpha");
    T_OK(ps_alloc(s, &b, &p));
    put_text(p, "beta");
    ps_op_commit(s);
    T_EQ(a.raw, 1);
    T_EQ(b.raw, 2);
    T_OK(ps_op_begin(s, 11));
    T_OK(ps_write(s, a, &p));
    put_text(p, "alpha2");
    ps_op_commit(s);
    T_OK(ps_checkpoint(s));
    T_EQ(ps_durable_lsn(s), 11);
    /* This store has no WAL, so reopen sees just the last checkpoint. */
    T_OK(ps_op_begin(s, 12));
    T_OK(ps_free(s, b));
    ps_op_commit(s);
    ps_close(s);

    T_OK(ps_open(dir, &cfg, 0, &s));
    T_EQ(ps_durable_lsn(s), 11);
    T_EQ(ps_committed_lsn(s), 11);
    check_text(s, a, "alpha2");
    check_text(s, b, "beta");
    struct ps_stats st;
    ps_stats(s, &st);
    T_EQ(st.allocated_pages, 2);
    ps_close(s);

    T_OK(ps_open(dir, &cfg, PS_CREATE, &s));
    T_EQ(ps_durable_lsn(s), 11);
    check_text(s, a, "alpha2");
    ps_close(s);
    teardown();
}

/* Open twice in one process to exercise flock on separate descriptors. */
static void test_second_open_ebusy(void)
{
    setup();
    const ps_config cfg = file_config(0);
    ps_store *s1 = NULL;
    ps_store *s2 = NULL;
    T_OK(ps_open(dir, &cfg, PS_CREATE, &s1));
    T_ERR(ps_open(dir, &cfg, 0, &s2), EBUSY);
    T_ERR(ps_open(dir, &cfg, PS_CREATE, &s2), EBUSY);
    T_CHECK(s2 == NULL);
    /* A trailing slash names the same directory. */
    char alias[4200];
    snprintf(alias, sizeof(alias), "%s/", dir);
    T_ERR(ps_open(alias, &cfg, 0, &s2), EBUSY);
    ps_page_id id = PS_PAGE_ID_NONE;
    ps_page *p = NULL;
    T_OK(ps_op_begin(s1, 1));
    T_OK(ps_alloc(s1, &id, &p));
    put_text(p, "one");
    ps_op_commit(s1);
    T_OK(ps_checkpoint(s1));
    ps_close(s1);
    T_OK(ps_open(dir, &cfg, 0, &s2));
    check_text(s2, id, "one");
    ps_close(s2);
    teardown();
}

static void test_page_size_mismatch(void)
{
    setup();
    ps_config cfg = file_config(8192);
    ps_store *s = NULL;
    T_OK(ps_open(dir, &cfg, PS_CREATE, &s));
    T_EQ(ps_page_size(s), 8192);
    ps_close(s);

    cfg.page_size = 4096;
    T_ERR(ps_open(dir, &cfg, 0, &s), EINVAL);
    T_ERR(ps_open(dir, &cfg, PS_CREATE, &s), EINVAL);
    cfg.page_size = 0;
    T_OK(ps_open(dir, &cfg, 0, &s));
    T_EQ(ps_page_size(s), 8192);
    ps_close(s);
    cfg.page_size = 8192;
    T_OK(ps_open(dir, &cfg, 0, &s));
    T_EQ(ps_page_size(s), 8192);
    ps_close(s);
    /* A bad config must leave the directory absent. */
    cfg.page_size = 5000;
    T_ERR(ps_open(dir, &cfg, 0, &s), EINVAL);
    teardown();
}

static void test_open_missing_enoent(void)
{
    setup();
    const ps_config cfg = file_config(0);
    ps_store *s = NULL;
    T_ERR(ps_open(root, &cfg, 0, &s), ENOENT); /* an empty directory */
    char data[4200];
    snprintf(data, sizeof(data), "%s/data", root);
    T_CHECK(!exists(data));
    T_OK(ps_open(root, &cfg, PS_CREATE, &s));
    ps_close(s);
    T_OK(ps_open(root, &cfg, 0, &s));
    ps_close(s);
    teardown();
}

static void test_bad_args(void)
{
    setup();
    const ps_config cfg = file_config(0);
    ps_store *s = NULL;
    T_ERR(ps_open("", &cfg, PS_CREATE, &s), EINVAL);
    T_ERR(ps_open(NULL, &cfg, PS_CREATE, &s), EINVAL);
    ps_config bad = cfg;
    bad.cache_size = 4096; /* fewer than 16 frames */
    T_ERR(ps_open(dir, &bad, PS_CREATE, &s), EINVAL);
    teardown();
}

/* Backend-only stores have no directory. Both opens copy the VFS table. */
static void test_accessors(void)
{
    setup();
    const ps_config cfg = file_config(0);
    ps_vfs vfs;
    ps_vfs_posix_init(&vfs);
    ps_backend be;
    T_OK(ps_file_backend_new(dir, &vfs, &be));
    ps_store *s = NULL;
    T_OK(ps_open_backend(&be, &cfg, PS_CREATE, &s));
    T_CHECK(ps_store_dir(s) == NULL);
    const ps_vfs *sv = ps_store_vfs(s);
    T_CHECK(sv != NULL && sv->open == vfs.open);
    ps_close(s);

    /* Clearing the caller's VFS exercises the store's copy. */
    ps_config c2 = cfg;
    c2.vfs = &vfs;
    T_OK(ps_open(dir, &c2, 0, &s));
    T_CHECK(ps_store_vfs(s) != &vfs);
    T_CHECK(ps_store_vfs(s)->open == vfs.open);
    T_CHECK(strcmp(ps_store_dir(s), dir) == 0);
    ps_close(s);
    teardown();
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_create_reopen);
    T_RUN(test_second_open_ebusy);
    T_RUN(test_page_size_mismatch);
    T_RUN(test_open_missing_enoent);
    T_RUN(test_bad_args);
    T_RUN(test_accessors);
    return t_done();
}
