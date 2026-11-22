#ifndef PAGESTORE_VFS_H
#define PAGESTORE_VFS_H

/*
 * File-system interface used by the file backend and the WAL. Tests swap in
 * a fault-injecting implementation; ps_vfs_posix() is the real one.
 *
 * Every function returning int returns 0 on success or a positive errno
 * value on failure. ctx is the vfs's own ctx field, passed back unchanged.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ps_file ps_file;

#define PS_VFS_CREATE 0x1u /* create if missing */
#define PS_VFS_EXCL   0x2u /* with CREATE: EEXIST if it exists */

typedef struct ps_vfs {
    void *ctx;
    /* Opens path read-write. ENOENT if missing and PS_VFS_CREATE is not set. */
    int (*open)(void *ctx, const char *path, unsigned flags, ps_file **out);
    /* Also releases a lock acquired through lock. */
    void (*close)(void *ctx, ps_file *f);
    /* A successful short read means EOF. */
    int (*pread)(void *ctx, ps_file *f, void *buf, size_t len, uint64_t off, size_t *got);
    /* Success means all len bytes were written. */
    int (*pwrite)(void *ctx, ps_file *f, const void *buf, size_t len, uint64_t off);
    /* Sync file data and size. Use fsync_dir for directory entries. */
    int (*fsync)(void *ctx, ps_file *f);
    int (*size)(void *ctx, ps_file *f, uint64_t *out);
    int (*truncate)(void *ctx, ps_file *f, uint64_t size);
    /* Reserve blocks for [0, size) and extend the file if needed. */
    int (*preallocate)(void *ctx, ps_file *f, uint64_t size);
    int (*unlink)(void *ctx, const char *path);
    /* Paths must share a directory. An existing target is atomically replaced. */
    int (*rename)(void *ctx, const char *from, const char *to);
    /* EEXIST if path exists. */
    int (*mkdir)(void *ctx, const char *path);
    /* Makes the directory's entries durable. */
    int (*fsync_dir)(void *ctx, const char *path);
    /*
     * Skip "." and "..". Return the first nonzero callback result and
     * stop listing.
     */
    int (*list)(void *ctx, const char *dir, int (*fn)(void *arg, const char *name), void *arg);
    /*
     * Creates path if missing and takes an exclusive, non-blocking lock on it.
     * EBUSY if it is held, including through another handle in this process.
     * close() on the handle releases the lock.
     */
    int (*lock)(void *ctx, const char *path, ps_file **out);
} ps_vfs;

/* The POSIX implementation (ctx is unused). */
const ps_vfs *ps_vfs_posix(void);

#ifdef __cplusplus
}
#endif

#endif /* PAGESTORE_VFS_H */
