/*
 * The POSIX VFS keeps a file descriptor in each handle. Positioned reads
 * and writes loop over partial transfers.
 */

#include "pagestore/vfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct ps_file {
    int fd;
};

#define FILE_MODE 0644
#define DIR_MODE  0755

/* The current errno as a return code; never 0. */
static int err(void)
{
    return errno != 0 ? errno : EIO;
}

/* Check the end offset before converting to signed off_t. */
static int check_range(uint64_t off, size_t len)
{
    const uint64_t max = (uint64_t)INT64_MAX;
    if (off > max || (uint64_t)len > max - off)
        return EFBIG;
    return 0;
}

static int wrap_fd(int fd, ps_file **out)
{
    ps_file *f = malloc(sizeof(*f));
    if (f == NULL) {
        close(fd);
        return ENOMEM;
    }
    f->fd = fd;
    *out = f;
    return 0;
}

static int sync_fd(int fd, int is_dir)
{
#ifdef __APPLE__
    (void)is_dir;
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return 0;
    /* Network and FUSE mounts can reject F_FULLFSYNC. Fall back to fsync. */
    if (errno != ENOTSUP && errno != EINVAL)
        return err();
    if (fsync(fd) == 0)
        return 0;
    return err();
#else
    /* fdatasync includes file size changes. Directory entries need fsync. */
    int r = is_dir ? fsync(fd) : fdatasync(fd);
    return r == 0 ? 0 : err();
#endif
}

static int posix_open(void *ctx, const char *path, unsigned flags, ps_file **out)
{
    (void)ctx;
    int oflags = O_RDWR | O_CLOEXEC;
    if (flags & PS_VFS_CREATE) {
        oflags |= O_CREAT;
        if (flags & PS_VFS_EXCL)
            oflags |= O_EXCL;
    }
    int fd;
    do
        fd = open(path, oflags, FILE_MODE);
    while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return err();
    return wrap_fd(fd, out);
}

static void posix_close(void *ctx, ps_file *f)
{
    (void)ctx;
    if (f == NULL)
        return;
    /* Retrying close after EINTR could close a reused descriptor. */
    close(f->fd);
    free(f);
}

static int posix_pread(void *ctx, ps_file *f, void *buf, size_t len, uint64_t off, size_t *got)
{
    (void)ctx;
    int r = check_range(off, len);
    if (r != 0)
        return r;
    uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(f->fd, p + done, len - done, (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return err();
        }
        if (n == 0)
            break;
        done += (size_t)n;
    }
    *got = done;
    return 0;
}

static int posix_pwrite(void *ctx, ps_file *f, const void *buf, size_t len, uint64_t off)
{
    (void)ctx;
    int r = check_range(off, len);
    if (r != 0)
        return r;
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(f->fd, p + done, len - done, (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return err();
        }
        if (n == 0)
            return EIO;
        done += (size_t)n;
    }
    return 0;
}

static int posix_fsync(void *ctx, ps_file *f)
{
    (void)ctx;
    return sync_fd(f->fd, 0);
}

static int posix_size(void *ctx, ps_file *f, uint64_t *out)
{
    (void)ctx;
    struct stat st;
    if (fstat(f->fd, &st) != 0)
        return err();
    *out = (uint64_t)st.st_size;
    return 0;
}

static int posix_truncate(void *ctx, ps_file *f, uint64_t size)
{
    (void)ctx;
    int r = check_range(size, 0);
    if (r != 0)
        return r;
    while (ftruncate(f->fd, (off_t)size) != 0) {
        if (errno != EINTR)
            return err();
    }
    return 0;
}

static int posix_preallocate(void *ctx, ps_file *f, uint64_t size)
{
    (void)ctx;
    int r = check_range(size, 0);
    if (r != 0)
        return r;
    if (size == 0)
        return 0;
#ifdef __APPLE__
    struct stat st;
    if (fstat(f->fd, &st) != 0)
        return err();
    if ((uint64_t)st.st_size >= size)
        return 0;
    /*
     * F_PEOFPOSMODE starts at the physical EOF. Request the logical
     * shortfall, then extend the file to the requested size.
     */
    fstore_t fs;
    memset(&fs, 0, sizeof(fs));
    fs.fst_flags = F_ALLOCATECONTIG | F_ALLOCATEALL;
    fs.fst_posmode = F_PEOFPOSMODE;
    fs.fst_offset = 0;
    fs.fst_length = (off_t)(size - (uint64_t)st.st_size);
    if (fcntl(f->fd, F_PREALLOCATE, &fs) == -1) {
        fs.fst_flags = F_ALLOCATEALL;
        if (fcntl(f->fd, F_PREALLOCATE, &fs) == -1)
            return err();
    }
    while (ftruncate(f->fd, (off_t)size) != 0) {
        if (errno != EINTR)
            return err();
    }
    return 0;
#else
    /* Mode 0 grows the file to cover the allocation and preserves larger sizes. */
    for (;;) {
        if (fallocate(f->fd, 0, 0, (off_t)size) == 0)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno != EOPNOTSUPP && errno != ENOSYS)
            return err();
        break;
    }
    /* libc can emulate allocation on filesystems without fallocate. */
    do
        r = posix_fallocate(f->fd, 0, (off_t)size);
    while (r == EINTR);
    return r;
#endif
}

static int posix_unlink(void *ctx, const char *path)
{
    (void)ctx;
    return unlink(path) == 0 ? 0 : err();
}

static int posix_rename(void *ctx, const char *from, const char *to)
{
    (void)ctx;
    return rename(from, to) == 0 ? 0 : err();
}

static int posix_mkdir(void *ctx, const char *path)
{
    (void)ctx;
    return mkdir(path, DIR_MODE) == 0 ? 0 : err();
}

static int posix_fsync_dir(void *ctx, const char *path)
{
    (void)ctx;
    int fd;
    do
        fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return err();
    int r = sync_fd(fd, 1);
    close(fd);
    return r;
}

static int posix_list(void *ctx, const char *dir, int (*fn)(void *arg, const char *name),
                      void *arg)
{
    (void)ctx;
    DIR *d = opendir(dir);
    if (d == NULL)
        return err();
    int r = 0;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(d);
        if (de == NULL) {
            if (errno != 0)
                r = errno;
            break;
        }
        const char *name = de->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        r = fn(arg, name);
        if (r != 0)
            break;
    }
    closedir(d);
    return r;
}

static int posix_lock(void *ctx, const char *path, ps_file **out)
{
    (void)ctx;
    int fd;
    do
        fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, FILE_MODE);
    while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return err();
    /*
     * flock follows the open file description. A separate open of the same
     * file conflicts with this lock, and closing that handle leaves it held.
     */
    while (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EINTR)
            continue;
        int e = (errno == EWOULDBLOCK || errno == EAGAIN) ? EBUSY : err();
        close(fd);
        return e;
    }
    return wrap_fd(fd, out);
}

static const ps_vfs posix_vfs = {
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

const ps_vfs *ps_vfs_posix(void)
{
    return &posix_vfs;
}
