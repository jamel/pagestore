/*
 * The POSIX VFS keeps a file descriptor in each handle. Positioned reads
 * and writes loop over partial transfers.
 */

#include "pagestore/vfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/* What this implementation keeps in a ps_file. */
typedef struct posix_file {
    int fd;
} posix_file;

PS_STATIC_ASSERT(sizeof(posix_file) <= sizeof(ps_file), "posix_file must fit in ps_file");

#define FILE_MODE 0644
#define DIR_MODE 0755

static inline posix_file *pf(ps_file *f)
{
    return (posix_file *) f->opaque;
}

static void set_fd(ps_file *f, int fd)
{
    memset(f, 0, sizeof(*f));
    pf(f)->fd = fd;
}

/* Return a positive errno even if the underlying call left errno unset. */
static int err(int rv)
{
    return rv != 0 ? rv : EIO;
}

/* Check the end offset before converting to signed off_t. */
static int check_range(u64 off, usize len)
{
    const u64 max = (u64) INT64_MAX;
    if (off > max || (u64) len > max - off) {
        return EFBIG;
    }
    return 0;
}

/* Retry open on EINTR. On failure, return -1 and put the error in *rv. */
static int open_retry(const char *path, int oflags, int *rv)
{
    for (;;) {
        const int fd = open(path, oflags, FILE_MODE);
        if (fd >= 0) {
            return fd;
        }
        const int e = errno;
        if (e != EINTR) {
            *rv = err(e);
            return -1;
        }
    }
}

/* The caller has already checked that size fits off_t. */
static int ftruncate_retry(int fd, u64 size)
{
    for (;;) {
        if (ftruncate(fd, (off_t) size) == 0) {
            return 0;
        }
        const int rv = errno;
        if (rv != EINTR) {
            return err(rv);
        }
    }
}

/* Sync file data and size, or directory entries when is_dir is set. */
static int sync_fd(int fd, bool is_dir)
{
#ifdef __APPLE__
    PS_UNUSED(is_dir);
    if (fcntl(fd, F_FULLFSYNC) == 0) {
        return 0;
    }
    const int rv = errno;
    /* Network and FUSE mounts can reject F_FULLFSYNC. Fall back to fsync. */
    if (rv != ENOTSUP && rv != EINVAL) {
        return err(rv);
    }
    if (fsync(fd) == 0) {
        return 0;
    }
    return err(errno);
#else
    /* fdatasync includes file size changes. Directory entries need fsync. */
    if ((is_dir ? fsync(fd) : fdatasync(fd)) == 0) {
        return 0;
    }
    return err(errno);
#endif
}

static int posix_open(ps_vfs_ctx *ctx, const char *path, u32 flags, ps_file *out)
{
    PS_UNUSED(ctx);
    int oflags = O_RDWR | O_CLOEXEC;
    if (flags & PS_VFS_CREATE) {
        oflags |= O_CREAT;
        if (flags & PS_VFS_EXCL) {
            oflags |= O_EXCL;
        }
    }
    int rv = 0;
    const int fd = open_retry(path, oflags, &rv);
    if (fd < 0) {
        return rv;
    }
    set_fd(out, fd);
    return 0;
}

static void posix_close(ps_vfs_ctx *ctx, ps_file *f)
{
    PS_UNUSED(ctx);
    if (f == NULL) {
        return;
    }
    /* Retrying close after EINTR could close a reused descriptor. */
    close(pf(f)->fd);
    set_fd(f, -1);
}

static int posix_pread(ps_vfs_ctx *ctx, ps_file *f, void *buf, usize len, u64 off, usize *got)
{
    PS_UNUSED(ctx);
    const int r = check_range(off, len);
    if (r != 0) {
        return r;
    }
    u8 *p = buf;
    usize done = 0;
    while (done < len) {
        const isize n = pread(pf(f)->fd, p + done, len - done, (off_t) (off + done));
        if (n < 0) {
            const int rv = errno;
            if (rv == EINTR) {
                continue;
            }
            return err(rv);
        }
        if (n == 0) {
            break;
        }
        done += (usize) n;
    }
    *got = done;
    return 0;
}

static int posix_pwrite(ps_vfs_ctx *ctx, ps_file *f, const void *buf, usize len, u64 off)
{
    PS_UNUSED(ctx);
    const int r = check_range(off, len);
    if (r != 0) {
        return r;
    }
    const u8 *p = buf;
    usize done = 0;
    while (done < len) {
        const isize n = pwrite(pf(f)->fd, p + done, len - done, (off_t) (off + done));
        if (n < 0) {
            const int rv = errno;
            if (rv == EINTR) {
                continue;
            }
            return err(rv);
        }
        if (n == 0) {
            return EIO;
        }
        done += (usize) n;
    }
    return 0;
}

static int posix_fsync(ps_vfs_ctx *ctx, ps_file *f)
{
    PS_UNUSED(ctx);
    return sync_fd(pf(f)->fd, false);
}

static int posix_size(ps_vfs_ctx *ctx, ps_file *f, u64 *out)
{
    PS_UNUSED(ctx);
    struct stat st;
    if (fstat(pf(f)->fd, &st) != 0) {
        return err(errno);
    }
    *out = (u64) st.st_size;
    return 0;
}

static int posix_truncate(ps_vfs_ctx *ctx, ps_file *f, u64 size)
{
    PS_UNUSED(ctx);
    const int r = check_range(size, 0);
    if (r != 0) {
        return r;
    }
    return ftruncate_retry(pf(f)->fd, size);
}

static int posix_preallocate(ps_vfs_ctx *ctx, ps_file *f, u64 size)
{
    PS_UNUSED(ctx);
    int r = check_range(size, 0);
    if (r != 0) {
        return r;
    }
    if (size == 0) {
        return 0;
    }
    const int fd = pf(f)->fd;
#ifdef __APPLE__
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return err(errno);
    }
    if ((u64) st.st_size >= size) {
        return 0;
    }
    /*
     * F_PEOFPOSMODE starts at the physical EOF. Request the logical
     * shortfall, then extend the file to the requested size.
     */
    fstore_t fs;
    memset(&fs, 0, sizeof(fs));
    fs.fst_flags = F_ALLOCATECONTIG | F_ALLOCATEALL;
    fs.fst_posmode = F_PEOFPOSMODE;
    fs.fst_offset = 0;
    fs.fst_length = (off_t) (size - (u64) st.st_size);
    if (fcntl(fd, F_PREALLOCATE, &fs) == -1) {
        fs.fst_flags = F_ALLOCATEALL;
        if (fcntl(fd, F_PREALLOCATE, &fs) == -1) {
            return err(errno);
        }
    }
    return ftruncate_retry(fd, size);
#else
    /* Mode 0 grows the file to cover the allocation and preserves larger sizes. */
    for (;;) {
        if (fallocate(fd, 0, 0, (off_t) size) == 0) {
            return 0;
        }
        const int rv = errno;
        if (rv == EINTR) {
            continue;
        }
        if (rv != EOPNOTSUPP && rv != ENOSYS) {
            return err(rv);
        }
        break;
    }
    /* libc can emulate allocation on filesystems without fallocate. */
    do {
        r = posix_fallocate(fd, 0, (off_t) size);
    } while (r == EINTR);
    return r;
#endif
}

static int posix_unlink(ps_vfs_ctx *ctx, const char *path)
{
    PS_UNUSED(ctx);
    return unlink(path) == 0 ? 0 : err(errno);
}

static int posix_rename(ps_vfs_ctx *ctx, const char *from, const char *to)
{
    PS_UNUSED(ctx);
    return rename(from, to) == 0 ? 0 : err(errno);
}

static int posix_mkdir(ps_vfs_ctx *ctx, const char *path)
{
    PS_UNUSED(ctx);
    return mkdir(path, DIR_MODE) == 0 ? 0 : err(errno);
}

static int posix_fsync_dir(ps_vfs_ctx *ctx, const char *path)
{
    PS_UNUSED(ctx);
    int rv = 0;
    const int fd = open_retry(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC, &rv);
    if (fd < 0) {
        return rv;
    }
    rv = sync_fd(fd, true);
    close(fd);
    return rv;
}

static int posix_list(ps_vfs_ctx *ctx, const char *dir, ps_vfs_list_fn fn, ps_vfs_list_ctx *arg)
{
    PS_UNUSED(ctx);
    DIR *d = opendir(dir);
    if (d == NULL) {
        return err(errno);
    }
    int r = 0;
    for (;;) {
        errno = 0;
        const struct dirent *de = readdir(d);
        if (de == NULL) {
            const int rv = errno;
            if (rv != 0) {
                r = rv;
            }
            break;
        }
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        r = fn(arg, name);
        if (r != 0) {
            break;
        }
    }
    closedir(d);
    return r;
}

static int posix_lock(ps_vfs_ctx *ctx, const char *path, ps_file *out)
{
    PS_UNUSED(ctx);
    int rv = 0;
    const int fd = open_retry(path, O_RDWR | O_CREAT | O_CLOEXEC, &rv);
    if (fd < 0) {
        return rv;
    }
    /*
     * flock follows the open file description. A separate open of the same
     * file conflicts with this lock, and closing that handle leaves it held.
     */
    while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        rv = errno;
        if (rv == EINTR) {
            continue;
        }
        close(fd);
        return (rv == EWOULDBLOCK || rv == EAGAIN) ? EBUSY : err(rv);
    }
    set_fd(out, fd);
    return 0;
}

void ps_vfs_posix_init(ps_vfs *vfs)
{
    *vfs = (ps_vfs) {
        .ctx = NULL,
        .open = posix_open,
        .close = posix_close,
        .pread = posix_pread,
        .pwrite = posix_pwrite,
        .fsync = posix_fsync,
        .size = posix_size,
        .truncate = posix_truncate,
        .preallocate = posix_preallocate,
        .unlink = posix_unlink,
        .rename = posix_rename,
        .mkdir = posix_mkdir,
        .fsync_dir = posix_fsync_dir,
        .list = posix_list,
        .lock = posix_lock,
    };
}
