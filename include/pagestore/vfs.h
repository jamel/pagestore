#pragma once

/*
 * File-system calls go through this table so tests can supply their own
 * implementation. Integer callbacks return 0 or a positive errno.
 *
 * File contents and directory entries have separate syncs. In an already
 * durable directory, sync the new file and then the directory itself.
 */

#include "pagestore/compiler.h"
#include "pagestore/types.h"

PS_EXTERN_C_BEGIN

/* The caller owns the storage. Its VFS owns the contents. */
typedef struct ps_file {
    u64 opaque[2];
} ps_file;

/* Defined by the VFS. */
typedef struct ps_vfs_ctx ps_vfs_ctx;

/* Defined by the caller of list. */
typedef struct ps_vfs_list_ctx ps_vfs_list_ctx;

/* name is the entry's name within the directory. Consume it during the call. */
typedef int (*ps_vfs_list_fn)(ps_vfs_list_ctx *arg, const char *name);

#define PS_VFS_CREATE 0x1u /* create if missing */
#define PS_VFS_EXCL 0x2u /* with CREATE: EEXIST if it exists */

typedef struct ps_vfs {
    ps_vfs_ctx *ctx;
    /* Open read-write. PS_VFS_CREATE preserves existing contents. */
    int (*open)(ps_vfs_ctx *ctx, const char *path, u32 flags, ps_file *out);
    /* Also releases a lock acquired through lock. */
    void (*close)(ps_vfs_ctx *ctx, ps_file *f);
    /* A successful short read means EOF. */
    int (*pread)(ps_vfs_ctx *ctx, ps_file *f, void *buf, usize len, u64 off, usize *got);
    /* Success means all len bytes were written. */
    int (*pwrite)(ps_vfs_ctx *ctx, ps_file *f, const void *buf, usize len, u64 off);
    /* Sync file data and size. Use fsync_dir for directory entries. */
    int (*fsync)(ps_vfs_ctx *ctx, ps_file *f);
    int (*size)(ps_vfs_ctx *ctx, ps_file *f, u64 *out);
    int (*truncate)(ps_vfs_ctx *ctx, ps_file *f, u64 size);
    /* Reserve blocks for [0, size) and extend the file if needed. */
    int (*preallocate)(ps_vfs_ctx *ctx, ps_file *f, u64 size);
    int (*unlink)(ps_vfs_ctx *ctx, const char *path);
    /* Paths must share a directory. An existing target is atomically replaced. */
    int (*rename)(ps_vfs_ctx *ctx, const char *from, const char *to);
    /* EEXIST if path exists. */
    int (*mkdir)(ps_vfs_ctx *ctx, const char *path);
    /* Makes the directory's entries durable. */
    int (*fsync_dir)(ps_vfs_ctx *ctx, const char *path);
    /*
     * Skip "." and "..". Return the first nonzero callback result and
     * stop listing.
     */
    int (*list)(ps_vfs_ctx *ctx, const char *dir, ps_vfs_list_fn fn, ps_vfs_list_ctx *arg);
    /*
     * Create path if missing. Lock exclusively and return EBUSY if held,
     * even by another handle in this process.
     */
    int (*lock)(ps_vfs_ctx *ctx, const char *path, ps_file *out);
} ps_vfs;

/* Installs the POSIX implementation. Its context is NULL. */
void ps_vfs_posix_init(ps_vfs *vfs);

PS_EXTERN_C_END
