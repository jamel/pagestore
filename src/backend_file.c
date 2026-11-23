/*
 * The file backend holds the store's LOCK and checkpoints through a
 * double-write journal. The syncs put the disk writes in this order.
 *
 *     write journal -> sync journal -> update data -> sync data -> clear journal
 *
 * Reopen can finish a checkpoint from a durable journal even if the data
 * pages or header were only partly written.
 *
 * Without a durable directory entry for the journal, recovery would keep
 * torn pages, so each open syncs the directory before its first checkpoint.
 * Replay syncs the journal and directory before touching data, because a
 * failed journal fsync can leave the journal readable but not durable.
 */

#include "backend_file.h"

#include "crc32c.h"
#include "format.h"
#include "util.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define FB_BUF_SIZE ((usize) 1 << 20)
#define FB_DEFAULT_PAGE_SIZE 4096u

PS_STATIC_ASSERT(
    FB_BUF_SIZE >= PS_JOURNAL_HEADER_SIZE + PS_JOURNAL_ENTRY_HEADER_SIZE + PS_PAGE_SIZE_MAX +
            PS_JOURNAL_TRAILER_SIZE,
    "the I/O buffer must hold the journal header, one entry and the trailer");

typedef enum fb_state {
    FB_IDLE,
    FB_ACTIVE,
    FB_FAILED /* reopen to recover */
} fb_state;

struct ps_backend_ctx {
    ps_vfs vfs;
    char *dir;
    char *parent;
    char *lock_path;
    char *data_path;
    char *creating_path;
    char *journal_path;

    ps_file lock;
    ps_file data;
    ps_file journal;
    bool have_lock;
    bool have_data;
    bool have_journal;
    bool opened;
    bool dir_synced; /* the journal and data entries are durable */

    /* read_page uses this field and the data handle, both set at open. */
    u32 page_size;

    /* Only checkpoint code updates the durable header after open. */
    u32 num_groups;
    u64 seq;
    u64 lsn;

    fb_state state;
    ps_journal_header jh;
    u32 written;
    u64 joff;
    u32 crc; /* CRC32C excludes bytes still in the buffer */
    u8 *buf;
    usize buf_len;
};

/* ---- paths ---- */

static char *str_join(const char *a, const char *b)
{
    const usize la = strlen(a);
    const usize lb = strlen(b);
    char *s = malloc(la + lb + 1);
    if (s != NULL) {
        memcpy(s, a, la);
        memcpy(s + la, b, lb + 1);
    }
    return s;
}

static char *path_join(const char *dir, const char *sep, const char *name)
{
    char *a = str_join(dir, sep);
    if (a == NULL) {
        return NULL;
    }
    char *s = str_join(a, name);
    free(a);
    return s;
}

/* For example, "/a/b" gives "/a", "/a" gives "/", and "a" gives ".". */
static char *parent_of(const char *path)
{
    usize n = strlen(path);
    while (n > 1 && path[n - 1] == '/') {
        n--;
    }
    while (n > 0 && path[n - 1] != '/') {
        n--;
    }
    if (n == 0) {
        return str_join(".", "");
    }
    while (n > 1 && path[n - 1] == '/') {
        n--;
    }
    char *s = malloc(n + 1);
    if (s != NULL) {
        memcpy(s, path, n);
        s[n] = '\0';
    }
    return s;
}

/* ---- small I/O helpers ---- */

/* The first page-sized slot holds the data header. */
static u64 page_off(u32 page_size, u32 id)
{
    return ((u64) id + 1) * (u64) page_size;
}

/* Read exactly len bytes. A short read means EBADMSG here. */
static int read_full(ps_backend_ctx *c, ps_file *f, void *buf, usize len, u64 off)
{
    usize got = 0;
    const int rv = c->vfs.pread(c->vfs.ctx, f, buf, len, off, &got);
    if (rv != 0) {
        return rv;
    }
    return got == len ? 0 : EBADMSG;
}

static int file_size(ps_backend_ctx *c, ps_file *f, u64 *out)
{
    return c->vfs.size(c->vfs.ctx, f, out);
}

static int write_header(ps_backend_ctx *c, ps_file *f, const ps_data_header *h)
{
    u8 hb[PS_DATA_HEADER_SIZE];
    ps_data_header_encode(h, hb);
    return c->vfs.pwrite(c->vfs.ctx, f, hb, sizeof(hb), 0);
}

static void close_file(ps_backend_ctx *c, ps_file *f, bool *have)
{
    if (*have) {
        c->vfs.close(c->vfs.ctx, f);
        *have = false;
    }
}

/* ---- journal ---- */

/* The caller wrote the journal or verified its CRC. Write the data header last. */
static int apply_journal(ps_backend_ctx *c, const ps_journal_header *jh)
{
    const usize stride = (usize) PS_JOURNAL_ENTRY_HEADER_SIZE + jh->page_size;
    const u32 batch = (u32) (FB_BUF_SIZE / stride);
    for (u32 i = 0; i < jh->count;) {
        const u32 n = PS_MIN(batch, jh->count - i);
        int rv = read_full(c, &c->journal, c->buf, (usize) n * stride, ps_journal_entry_off(jh, i));
        if (rv != 0) {
            return rv;
        }
        for (u32 j = 0; j < n; j++) {
            const u8 *e = c->buf + (usize) j * stride;
            u32 id = 0;
            rv = ps_journal_entry_header_decode(e, &id);
            if (rv != 0) {
                return rv;
            }
            if (id == UINT32_MAX) {
                return EBADMSG;
            }
            rv = c->vfs.pwrite(
                c->vfs.ctx,
                &c->data,
                e + PS_JOURNAL_ENTRY_HEADER_SIZE,
                jh->page_size,
                page_off(jh->page_size, id));
            if (rv != 0) {
                return rv;
            }
        }
        i += n;
    }
    const ps_data_header h = {
        .page_size = jh->page_size,
        .num_groups = jh->num_groups,
        .seq = jh->seq,
        .durable_lsn = jh->lsn,
    };
    int rv = write_header(c, &c->data, &h);
    if (rv != 0) {
        return rv;
    }
    return c->vfs.fsync(c->vfs.ctx, &c->data);
}

/* A malformed journal sets *valid to false. I/O errors stop recovery. */
static int check_journal(ps_backend_ctx *c, bool *valid, ps_journal_header *jh)
{
    *valid = false;
    u64 size = 0;
    int rv = file_size(c, &c->journal, &size);
    if (rv != 0) {
        return rv;
    }
    if (size < PS_JOURNAL_HEADER_SIZE) {
        return 0;
    }
    u8 hb[PS_JOURNAL_HEADER_SIZE];
    rv = read_full(c, &c->journal, hb, sizeof(hb), 0);
    if (rv != 0) {
        return rv;
    }
    if (ps_journal_header_decode(hb, jh) != 0 || ps_journal_size(jh) != size) {
        return 0;
    }
    const u64 end = ps_journal_entry_off(jh, jh->count);
    u32 crc = 0;
    for (u64 off = 0; off < end;) {
        const usize n = (usize) PS_MIN((u64) FB_BUF_SIZE, end - off);
        rv = read_full(c, &c->journal, c->buf, n, off);
        if (rv != 0) {
            return rv;
        }
        crc = ps_crc32c(crc, c->buf, n);
        off += n;
    }
    u8 trailer[PS_JOURNAL_TRAILER_SIZE];
    rv = read_full(c, &c->journal, trailer, sizeof(trailer), end);
    if (rv != 0) {
        return rv;
    }
    u32 want = 0;
    if (ps_journal_trailer_decode(trailer, &want) != 0 || want != crc) {
        return 0;
    }
    *valid = true;
    return 0;
}

static int truncate_journal(ps_backend_ctx *c)
{
    return c->vfs.truncate(c->vfs.ctx, &c->journal, 0);
}

/* Create a missing journal. The first checkpoint syncs its directory entry. */
static int open_journal(ps_backend_ctx *c)
{
    int rv = c->vfs.open(c->vfs.ctx, c->journal_path, 0, &c->journal);
    if (rv == ENOENT) {
        rv = c->vfs.open(c->vfs.ctx, c->journal_path, PS_VFS_CREATE, &c->journal);
    }
    if (rv == 0) {
        c->have_journal = true;
    }
    return rv;
}

/* ---- open ---- */

/* Put a durable header in data.creating before publishing it as data. */
static int format_data(ps_backend_ctx *c, u32 page_size)
{
    int rv = c->vfs.unlink(c->vfs.ctx, c->creating_path);
    if (rv != 0 && rv != ENOENT) {
        return rv;
    }
    rv = open_journal(c);
    if (rv != 0) {
        return rv;
    }
    ps_file f;
    rv = c->vfs.open(c->vfs.ctx, c->creating_path, PS_VFS_CREATE | PS_VFS_EXCL, &f);
    if (rv != 0) {
        return rv;
    }
    const ps_data_header h = {.page_size = page_size, .num_groups = 1, .seq = 0, .durable_lsn = 0};
    rv = write_header(c, &f, &h);
    if (rv == 0) {
        rv = c->vfs.fsync(c->vfs.ctx, &f);
    }
    c->vfs.close(c->vfs.ctx, &f);
    if (rv != 0) {
        return rv;
    }
    rv = c->vfs.rename(c->vfs.ctx, c->creating_path, c->data_path);
    if (rv != 0) {
        return rv;
    }
    /* This also syncs the journal entry created above. */
    rv = c->vfs.fsync_dir(c->vfs.ctx, c->dir);
    if (rv != 0) {
        return rv;
    }
    c->dir_synced = true;
    rv = c->vfs.open(c->vfs.ctx, c->data_path, 0, &c->data);
    if (rv != 0) {
        return rv;
    }
    c->have_data = true;
    c->page_size = h.page_size;
    c->num_groups = h.num_groups;
    c->seq = h.seq;
    c->lsn = h.durable_lsn;
    return 0;
}

/*
 * Replay a valid journal with a matching or newer sequence number. It can
 * also reconstruct the header when the data header is unreadable.
 */
static int recover(ps_backend_ctx *c)
{
    int rv = open_journal(c);
    if (rv != 0) {
        return rv;
    }
    u8 hb[PS_DATA_HEADER_SIZE];
    usize got = 0;
    rv = c->vfs.pread(c->vfs.ctx, &c->data, hb, sizeof(hb), 0, &got);
    if (rv != 0) {
        return rv;
    }
    ps_data_header h;
    const bool header_ok = got == sizeof(hb) && ps_data_header_decode(hb, &h) == 0;
    bool journal_ok = false;
    ps_journal_header jh;
    rv = check_journal(c, &journal_ok, &jh);
    if (rv != 0) {
        return rv;
    }
    if (header_ok && !(journal_ok && jh.seq >= h.seq)) {
        c->page_size = h.page_size;
        c->num_groups = h.num_groups;
        c->seq = h.seq;
        c->lsn = h.durable_lsn;
        return 0;
    }
    if (!journal_ok) {
        return EBADMSG;
    }
    if (header_ok && jh.page_size != h.page_size) {
        return EBADMSG;
    }
    /* After a failed fsync, the journal may exist only in the cache. */
    rv = c->vfs.fsync(c->vfs.ctx, &c->journal);
    if (rv == 0) {
        rv = c->vfs.fsync_dir(c->vfs.ctx, c->dir);
    }
    if (rv != 0) {
        return rv;
    }
    c->dir_synced = true;
    rv = apply_journal(c, &jh);
    if (rv != 0) {
        return rv;
    }
    rv = truncate_journal(c);
    if (rv != 0) {
        return rv;
    }
    c->page_size = jh.page_size;
    c->num_groups = jh.num_groups;
    c->seq = jh.seq;
    c->lsn = jh.lsn;
    return 0;
}

static void release_files(ps_backend_ctx *c)
{
    close_file(c, &c->journal, &c->have_journal);
    close_file(c, &c->data, &c->have_data);
    close_file(c, &c->lock, &c->have_lock);
}

static int fb_open_files(ps_backend_ctx *c, bool create, u32 page_size)
{
    if (create) {
        /* A prior open may have created dir and failed before syncing its parent. */
        const int rv = c->vfs.mkdir(c->vfs.ctx, c->dir);
        if (rv != 0 && rv != EEXIST) {
            return rv;
        }
        const int rv2 = c->vfs.fsync_dir(c->vfs.ctx, c->parent);
        if (rv2 != 0) {
            return rv2;
        }
    }
    int rv = c->vfs.lock(c->vfs.ctx, c->lock_path, &c->lock);
    if (rv != 0) {
        return rv;
    }
    c->have_lock = true;
    rv = c->vfs.open(c->vfs.ctx, c->data_path, 0, &c->data);
    if (rv == ENOENT) {
        return create ? format_data(c, page_size) : ENOENT;
    }
    if (rv != 0) {
        return rv;
    }
    c->have_data = true;
    return recover(c);
}

static int fb_open(ps_backend_ctx *c, bool create, u32 page_size, ps_backend_info *out)
{
    PS_ASSERT(!c->opened);
    if (c->opened) {
        return EINVAL;
    }
    if (create) {
        if (page_size == 0) {
            page_size = FB_DEFAULT_PAGE_SIZE;
        }
        if (!ps_page_size_valid(page_size)) {
            return EINVAL;
        }
    }
    const int rv = fb_open_files(c, create, page_size);
    if (rv != 0) {
        release_files(c);
        return rv;
    }
    c->opened = true;
    c->state = FB_IDLE;
    out->page_size = c->page_size;
    out->num_groups = c->num_groups;
    out->seq = c->seq;
    out->durable_lsn = c->lsn;
    return 0;
}

/* ---- read ---- */

static int fb_read_page(ps_backend_ctx *c, u32 id, void *buf)
{
    PS_ASSERT(c->opened && id != UINT32_MAX);
    if (!c->opened || id == UINT32_MAX) {
        return EINVAL;
    }
    usize got = 0;
    const int rv =
        c->vfs.pread(c->vfs.ctx, &c->data, buf, c->page_size, page_off(c->page_size, id), &got);
    if (rv != 0) {
        return rv;
    }
    if (got < c->page_size) {
        memset((u8 *) buf + got, 0, c->page_size - got);
    }
    return 0;
}

/* ---- checkpoint ---- */

/* Write the buffered bytes before adding them to the running CRC. */
static int flush_buf(ps_backend_ctx *c)
{
    if (c->buf_len == 0) {
        return 0;
    }
    const int rv = c->vfs.pwrite(c->vfs.ctx, &c->journal, c->buf, c->buf_len, c->joff);
    if (rv != 0) {
        return rv;
    }
    c->crc = ps_crc32c(c->crc, c->buf, c->buf_len);
    c->joff += c->buf_len;
    c->buf_len = 0;
    return 0;
}

static void reset_ckpt(ps_backend_ctx *c)
{
    c->buf_len = 0;
    c->joff = 0;
    c->crc = 0;
    c->written = 0;
}

static int fb_ckpt_begin(ps_backend_ctx *c, u64 lsn, u32 num_groups, u32 count)
{
    if (c->state == FB_FAILED) {
        return EIO;
    }
    PS_ASSERT(c->opened && c->state == FB_IDLE && num_groups > 0);
    if (!c->opened || c->state != FB_IDLE || num_groups == 0) {
        return EINVAL;
    }
    /* Nothing is written yet, so a failed sync can be retried. */
    if (!c->dir_synced) {
        const int rv = c->vfs.fsync_dir(c->vfs.ctx, c->dir);
        if (rv != 0) {
            return rv;
        }
        c->dir_synced = true;
    }
    /* Old trailing bytes would invalidate the new journal's length check. */
    u64 size = 0;
    int rv = file_size(c, &c->journal, &size);
    if (rv != 0) {
        return rv;
    }
    if (size != 0) {
        rv = truncate_journal(c);
        if (rv != 0) {
            return rv;
        }
    }
    reset_ckpt(c);
    c->jh = (ps_journal_header) {
        .page_size = c->page_size,
        .num_groups = num_groups,
        .count = count,
        .seq = c->seq + 1,
        .lsn = lsn,
    };
    ps_journal_header_encode(&c->jh, c->buf);
    c->buf_len = PS_JOURNAL_HEADER_SIZE;
    c->state = FB_ACTIVE;
    return 0;
}

static int fb_ckpt_write(ps_backend_ctx *c, u32 id, const void *page)
{
    PS_ASSERT(c->state == FB_ACTIVE && c->written < c->jh.count && id != UINT32_MAX);
    if (c->state != FB_ACTIVE || c->written >= c->jh.count || id == UINT32_MAX) {
        return EINVAL;
    }
    const usize entry = (usize) PS_JOURNAL_ENTRY_HEADER_SIZE + c->page_size;
    if (c->buf_len + entry > FB_BUF_SIZE) {
        const int rv = flush_buf(c);
        if (rv != 0) {
            return rv;
        }
    }
    ps_journal_entry_header_encode(id, c->buf + c->buf_len);
    memcpy(c->buf + c->buf_len + PS_JOURNAL_ENTRY_HEADER_SIZE, page, c->page_size);
    c->buf_len += entry;
    c->written++;
    return 0;
}

static int fb_ckpt_commit(ps_backend_ctx *c, bool *fatal)
{
    *fatal = false;
    PS_ASSERT(c->state == FB_ACTIVE && c->written == c->jh.count);
    if (c->state != FB_ACTIVE || c->written != c->jh.count) {
        return EINVAL;
    }
    /* A failed journal write leaves the data file untouched. */
    if (c->buf_len + PS_JOURNAL_TRAILER_SIZE > FB_BUF_SIZE) {
        const int rv = flush_buf(c);
        if (rv != 0) {
            return rv;
        }
    }
    ps_journal_trailer_encode(ps_crc32c(c->crc, c->buf, c->buf_len), c->buf + c->buf_len);
    c->buf_len += PS_JOURNAL_TRAILER_SIZE;
    int rv = flush_buf(c);
    if (rv != 0) {
        return rv;
    }
    PS_ASSERT(c->joff == ps_journal_size(&c->jh));

    /* Starting at this fsync, reopen to resolve any failure. */
    rv = c->vfs.fsync(c->vfs.ctx, &c->journal);
    if (rv == 0) {
        rv = apply_journal(c, &c->jh);
    }
    if (rv != 0) {
        c->state = FB_FAILED;
        *fatal = true;
        return rv;
    }
    /* The data is durable. If truncation fails, recovery can replay the journal again. */
    const int trv = truncate_journal(c);
    PS_UNUSED(trv);
    c->num_groups = c->jh.num_groups;
    c->seq = c->jh.seq;
    c->lsn = c->jh.lsn;
    reset_ckpt(c);
    c->state = FB_IDLE;
    return 0;
}

static void fb_ckpt_abort(ps_backend_ctx *c)
{
    PS_ASSERT(c->state == FB_ACTIVE);
    if (c->state != FB_ACTIVE) {
        return;
    }
    const int trv = truncate_journal(c);
    PS_UNUSED(trv);
    reset_ckpt(c);
    c->state = FB_IDLE;
}

/* ---- lifetime ---- */

static void fb_close(ps_backend_ctx *c)
{
    if (c == NULL) {
        return;
    }
    release_files(c);
    free(c->buf);
    free(c->dir);
    free(c->parent);
    free(c->lock_path);
    free(c->data_path);
    free(c->creating_path);
    free(c->journal_path);
    free(c);
}

int ps_file_backend_new(const char *dir, const ps_vfs *vfs, ps_backend *out)
{
    if (dir == NULL || dir[0] == '\0' || vfs == NULL) {
        return EINVAL;
    }
    ps_backend_ctx *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return ENOMEM;
    }
    c->vfs = *vfs;
    c->state = FB_IDLE;
    c->dir = str_join(dir, "");
    if (c->dir != NULL) {
        usize n = strlen(c->dir);
        while (n > 1 && c->dir[n - 1] == '/') {
            c->dir[--n] = '\0';
        }
    }
    c->parent = parent_of(dir);
    const char *d = c->dir != NULL ? c->dir : "";
    const char *sep = strcmp(d, "/") == 0 ? "" : "/";
    c->lock_path = path_join(d, sep, "LOCK");
    c->data_path = path_join(d, sep, "data");
    c->creating_path = path_join(d, sep, "data.creating");
    c->journal_path = path_join(d, sep, "journal");
    c->buf = malloc(FB_BUF_SIZE);
    if (c->dir == NULL || c->parent == NULL || c->lock_path == NULL || c->data_path == NULL ||
        c->creating_path == NULL || c->journal_path == NULL || c->buf == NULL) {
        fb_close(c);
        return ENOMEM;
    }
    *out = (ps_backend) {
        .ctx = c,
        .open = fb_open,
        .read_page = fb_read_page,
        .ckpt_begin = fb_ckpt_begin,
        .ckpt_write = fb_ckpt_write,
        .ckpt_commit = fb_ckpt_commit,
        .ckpt_abort = fb_ckpt_abort,
        .close = fb_close,
    };
    return 0;
}
