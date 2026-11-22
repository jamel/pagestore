#include "fault_vfs.h"

#include "rng.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE 4096u
#define TEAR_UNIT 512u
#define MAX_FILE_SIZE ((u64) INT64_MAX)

/* ---- allocation ---- */

static void oom(void)
{
    fprintf(stderr, "fault_vfs: out of memory\n");
    abort();
}

static void *xmalloc(usize n)
{
    void *p = malloc(n != 0 ? n : 1);
    if (p == NULL) {
        oom();
    }
    return p;
}

static void *xcalloc(usize n)
{
    void *p = calloc(1, n != 0 ? n : 1);
    if (p == NULL) {
        oom();
    }
    return p;
}

static void *xrealloc(void *p, usize n)
{
    void *q = realloc(p, n != 0 ? n : 1);
    if (q == NULL) {
        oom();
    }
    return q;
}

static char *xstrndup(const char *s, usize n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

/* cap and need are element counts. elem is the element size in bytes. */
static void *grow(void *p, usize *cap, usize need, usize elem)
{
    if (need <= *cap) {
        return p;
    }
    usize c = *cap != 0 ? *cap * 2 : 8;
    while (c < need) {
        c *= 2;
    }
    *cap = c;
    return xrealloc(p, c * elem);
}

static u64 min_u64(u64 a, u64 b)
{
    return a < b ? a : b;
}

/* ---- sparse file images ---- */

typedef struct block {
    u64 idx;
    u8 *data;
} block;

/* Stored bytes past EOF stay zero so a later extension reads as a hole. */
typedef struct image {
    u64 size;
    block *blocks; /* sorted by idx */
    usize n;
    usize cap;
} image;

/* Returns the insertion index for idx, including the end of the array. */
static usize image_lower_bound(const image *img, u64 idx)
{
    usize lo = 0;
    usize hi = img->n;
    while (lo < hi) {
        const usize mid = lo + (hi - lo) / 2;
        if (img->blocks[mid].idx < idx) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* Returns NULL for a hole unless the caller requests a new block. */
static u8 *image_block(image *img, u64 idx, bool create)
{
    const usize pos = image_lower_bound(img, idx);
    if (pos < img->n && img->blocks[pos].idx == idx) {
        return img->blocks[pos].data;
    }
    if (!create) {
        return NULL;
    }
    img->blocks = grow(img->blocks, &img->cap, img->n + 1, sizeof(*img->blocks));
    memmove(&img->blocks[pos + 1], &img->blocks[pos], (img->n - pos) * sizeof(*img->blocks));
    img->n++;
    img->blocks[pos].idx = idx;
    img->blocks[pos].data = xcalloc(BLOCK_SIZE);
    return img->blocks[pos].data;
}

static void image_free(image *img)
{
    for (usize i = 0; i < img->n; i++) {
        free(img->blocks[i].data);
    }
    free(img->blocks);
    memset(img, 0, sizeof(*img));
}

/* dst must be empty. */
static void image_copy(image *dst, const image *src)
{
    dst->size = src->size;
    dst->n = src->n;
    dst->cap = src->n;
    dst->blocks = src->n != 0 ? xmalloc(src->n * sizeof(*dst->blocks)) : NULL;
    for (usize i = 0; i < src->n; i++) {
        dst->blocks[i].idx = src->blocks[i].idx;
        dst->blocks[i].data = xmalloc(BLOCK_SIZE);
        memcpy(dst->blocks[i].data, src->blocks[i].data, BLOCK_SIZE);
    }
}

static void image_write(image *img, u64 off, const u8 *src, u64 len)
{
    const u64 end = off + len;
    while (len > 0) {
        const u64 in = off % BLOCK_SIZE;
        const u64 n = min_u64(BLOCK_SIZE - in, len);
        u8 *b = image_block(img, off / BLOCK_SIZE, true);
        memcpy(b + in, src, (usize) n);
        off += n;
        src += n;
        len -= n;
    }
    if (end > img->size) {
        img->size = end;
    }
}

/* Holes read as zeros. The count is short only at the end of the file. */
static usize image_read(image *img, u64 off, u8 *dst, usize len)
{
    if (off >= img->size) {
        return 0;
    }
    const u64 total = min_u64(len, img->size - off);
    u64 left = total;
    while (left > 0) {
        const u64 in = off % BLOCK_SIZE;
        const u64 n = min_u64(BLOCK_SIZE - in, left);
        const u8 *b = image_block(img, off / BLOCK_SIZE, false);
        if (b != NULL) {
            memcpy(dst, b + in, (usize) n);
        } else {
            memset(dst, 0, (usize) n);
        }
        off += n;
        dst += n;
        left -= n;
    }
    return (usize) total;
}

static void image_set_size(image *img, u64 size)
{
    if (size < img->size) {
        const u64 keep = size / BLOCK_SIZE + (size % BLOCK_SIZE != 0 ? 1 : 0);
        const usize pos = image_lower_bound(img, keep);
        for (usize i = pos; i < img->n; i++) {
            free(img->blocks[i].data);
        }
        img->n = pos;
        if (size % BLOCK_SIZE != 0) {
            u8 *b = image_block(img, size / BLOCK_SIZE, false);
            if (b != NULL) {
                const u64 in = size % BLOCK_SIZE;
                memset(b + in, 0, BLOCK_SIZE - in);
            }
        }
    }
    img->size = size;
}

/* ---- unsynced changes ---- */

typedef enum change_kind {
    CH_WRITE,
    CH_SETSIZE
} change_kind;

typedef struct change {
    change_kind kind;
    u64 off; /* New size for CH_SETSIZE */
    u64 len;
    u8 *data; /* Owned write bytes */
} change;

/* A torn write passes a shorter len than c->len. */
static void change_apply(image *img, const change *c, u64 len)
{
    if (c->kind == CH_WRITE) {
        image_write(img, c->off, c->data, len);
    } else {
        image_set_size(img, c->off);
    }
}

/* ---- directory entries and changes ---- */

struct node;

typedef struct entry {
    char *name;
    struct node *node;
} entry;

typedef struct entries {
    entry *v;
    usize n;
    usize cap;
} entries;

static isize ents_find(const entries *e, const char *name)
{
    for (usize i = 0; i < e->n; i++) {
        if (strcmp(e->v[i].name, name) == 0) {
            return (isize) i;
        }
    }
    return -1;
}

static struct node *ents_get(const entries *e, const char *name)
{
    const isize i = ents_find(e, name);
    return i >= 0 ? e->v[i].node : NULL;
}

/* Returns the displaced node so the caller can update its link count. */
static struct node *ents_set(entries *e, const char *name, struct node *n)
{
    const isize i = ents_find(e, name);
    if (i >= 0) {
        struct node *old = e->v[i].node;
        e->v[i].node = n;
        return old;
    }
    e->v = grow(e->v, &e->cap, e->n + 1, sizeof(*e->v));
    e->v[e->n].name = xstrdup(name);
    e->v[e->n].node = n;
    e->n++;
    return NULL;
}

/* The order of the remaining entries can change. */
static void ents_remove_at(entries *e, usize i)
{
    free(e->v[i].name);
    e->v[i] = e->v[e->n - 1];
    e->n--;
}

static void ents_free(entries *e)
{
    for (usize i = 0; i < e->n; i++) {
        free(e->v[i].name);
    }
    free(e->v);
    memset(e, 0, sizeof(*e));
}

/* dst must be empty. */
static void ents_copy(entries *dst, const entries *src)
{
    dst->n = src->n;
    dst->cap = src->n;
    dst->v = src->n != 0 ? xmalloc(src->n * sizeof(*dst->v)) : NULL;
    for (usize i = 0; i < src->n; i++) {
        dst->v[i].name = xstrdup(src->v[i].name);
        dst->v[i].node = src->v[i].node;
    }
}

typedef enum dirop_kind {
    OP_CREATE,
    OP_UNLINK,
    OP_RENAME
} dirop_kind;

typedef struct dirop {
    dirop_kind kind;
    char *name; /* Source name for OP_RENAME */
    char *to; /* RENAME only */
    struct node *node; /* CREATE only */
} dirop;

/* ---- nodes ---- */

typedef struct node {
    bool is_dir;
    bool locked;
    bool mark;
    u32 links; /* current directory entries naming it */
    u32 handles; /* open handles of the current boot */

    /* Files. */
    image cur;
    image dur;
    change *changes;
    usize nchanges;
    usize capchanges;

    /* Directories. */
    entries ents_cur;
    entries ents_dur;
    dirop *ops;
    usize nops;
    usize capops;

    struct node *next;
} node;

static void changes_clear(node *n)
{
    for (usize i = 0; i < n->nchanges; i++) {
        free(n->changes[i].data);
    }
    n->nchanges = 0;
}

static void ops_clear(node *n)
{
    for (usize i = 0; i < n->nops; i++) {
        free(n->ops[i].name);
        free(n->ops[i].to);
    }
    n->nops = 0;
}

static void node_free(node *n)
{
    image_free(&n->cur);
    image_free(&n->dur);
    changes_clear(n);
    free(n->changes);
    ents_free(&n->ents_cur);
    ents_free(&n->ents_dur);
    ops_clear(n);
    free(n->ops);
    free(n);
}

static void log_change(node *n, change_kind kind, u64 off, const u8 *data, u64 len)
{
    n->changes = grow(n->changes, &n->capchanges, n->nchanges + 1, sizeof(*n->changes));
    change *c = &n->changes[n->nchanges++];
    c->kind = kind;
    c->off = off;
    c->len = len;
    c->data = NULL;
    if (kind == CH_WRITE) {
        c->data = xmalloc((usize) len);
        memcpy(c->data, data, (usize) len);
    }
}

static void log_op(node *dir, dirop_kind kind, const char *name, const char *to, node *n)
{
    dir->ops = grow(dir->ops, &dir->capops, dir->nops + 1, sizeof(*dir->ops));
    dirop *op = &dir->ops[dir->nops++];
    op->kind = kind;
    op->name = xstrdup(name);
    op->to = to != NULL ? xstrdup(to) : NULL;
    op->node = n;
}

/* Current-namespace edits; they keep node.links up to date. */
static void cur_set(node *dir, const char *name, node *n)
{
    node *old = ents_set(&dir->ents_cur, name, n);
    if (old != NULL) {
        old->links--;
    }
    n->links++;
}

static void cur_remove(node *dir, const char *name)
{
    const isize i = ents_find(&dir->ents_cur, name);
    if (i >= 0) {
        dir->ents_cur.v[i].node->links--;
        ents_remove_at(&dir->ents_cur, (usize) i);
    }
}

/* ---- VFS state ---- */

struct fault_vfs {
    ps_vfs vfs;
    pthread_mutex_t mu; /* serializes everything below */
    rng rng;
    node *root;
    node *nodes; /* all nodes, in creation order */
    node *nodes_tail;
    u64 points;
    u64 crash_at;
    u64 fail_at;
    int fail_err;
    bool crashed;
    u64 space_limit;
    u32 epoch; /* incremented by reboot; handles of older epochs are stale */
};

struct ps_vfs_ctx {
    fault_vfs fv;
};

static fault_vfs *fv_of(ps_vfs_ctx *ctx)
{
    return &ctx->fv;
}

/* What this implementation keeps in a ps_file. */
typedef struct fv_file {
    node *node;
    u32 epoch;
    bool lock;
} fv_file;

PS_STATIC_ASSERT(sizeof(fv_file) <= sizeof(ps_file), "fv_file must fit in ps_file");

static inline fv_file *ff(ps_file *f)
{
    return (fv_file *) f->opaque;
}

static node *node_new(fault_vfs *fv, bool is_dir)
{
    node *n = xcalloc(sizeof(*n));
    n->is_dir = is_dir;
    if (fv->nodes_tail != NULL) {
        fv->nodes_tail->next = n;
    } else {
        fv->nodes = n;
    }
    fv->nodes_tail = n;
    return n;
}

static void gc_mark(node *n)
{
    if (n == NULL || n->mark) {
        return;
    }
    n->mark = true;
    if (!n->is_dir) {
        return;
    }
    for (usize i = 0; i < n->ents_cur.n; i++) {
        gc_mark(n->ents_cur.v[i].node);
    }
    for (usize i = 0; i < n->ents_dur.n; i++) {
        gc_mark(n->ents_dur.v[i].node);
    }
    for (usize i = 0; i < n->nops; i++) {
        gc_mark(n->ops[i].node);
    }
}

/*
 * A node can outlive its current name if a handle is open or reboot
 * could make the name visible again.
 */
static void gc(fault_vfs *fv)
{
    gc_mark(fv->root);
    for (node *n = fv->nodes; n != NULL; n = n->next) {
        if (n->handles > 0) {
            gc_mark(n);
        }
    }
    node *prev = NULL;
    node *n = fv->nodes;
    while (n != NULL) {
        node *next = n->next;
        if (n->mark) {
            n->mark = false;
            prev = n;
        } else {
            if (prev != NULL) {
                prev->next = next;
            } else {
                fv->nodes = next;
            }
            node_free(n);
        }
        n = next;
    }
    fv->nodes_tail = prev;
}

/* Call before making any changes. An injected fault takes precedence. */
static int count_point(fault_vfs *fv)
{
    fv->points++;
    if (fv->crash_at != 0 && fv->points == fv->crash_at) {
        fv->crashed = true;
        return EIO;
    }
    if (fv->fail_at != 0 && fv->points == fv->fail_at) {
        return fv->fail_err;
    }
    return 0;
}

/* ENOSPC if growing file n to new_size would exceed the limit. */
static int space_check(fault_vfs *fv, const node *n, u64 new_size)
{
    if (fv->space_limit == UINT64_MAX || new_size <= n->cur.size) {
        return 0;
    }
    u64 used = 0;
    for (const node *m = fv->nodes; m != NULL; m = m->next) {
        if (m == n || m->is_dir || (m->links == 0 && m->handles == 0)) {
            continue;
        }
        used = used > UINT64_MAX - m->cur.size ? UINT64_MAX : used + m->cur.size;
    }
    if (new_size > fv->space_limit || used > fv->space_limit - new_size) {
        return ENOSPC;
    }
    return 0;
}

/* A path resolved in the current namespace. */
typedef struct lookup {
    node *parent; /* NULL for "/" */
    char *name; /* last component (owned); NULL for "/" */
    node *node; /* NULL if missing */
} lookup;

static void lookup_free(lookup *l)
{
    free(l->name);
    l->name = NULL;
}

/* A missing final component is fine. Its parent may be used to create it. */
static int lookup_path(fault_vfs *fv, const char *path, lookup *out)
{
    memset(out, 0, sizeof(*out));
    if (path == NULL || path[0] != '/') {
        return EINVAL;
    }
    node *parent = NULL;
    node *n = fv->root;
    char *name = NULL;
    const char *p = path;
    for (;;) {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *e = p;
        while (*e != '\0' && *e != '/') {
            e++;
        }
        if (n == NULL || !n->is_dir) {
            free(name);
            return n == NULL ? ENOENT : ENOTDIR;
        }
        free(name);
        name = xstrndup(p, (usize) (e - p));
        parent = n;
        n = ents_get(&parent->ents_cur, name);
        p = e;
    }
    out->parent = parent;
    out->name = name;
    out->node = n;
    return 0;
}

/* Check the epoch before using a node that reboot may have freed. */
static int get_handle(fault_vfs *fv, ps_file *f, node **out)
{
    const fv_file *h = ff(f);
    if (h->node == NULL || h->epoch != fv->epoch) {
        return EBADF;
    }
    *out = h->node;
    return 0;
}

static void fill_handle(fault_vfs *fv, ps_file *out, node *n, bool lock)
{
    memset(out, 0, sizeof(*out));
    ff(out)->node = n;
    ff(out)->epoch = fv->epoch;
    ff(out)->lock = lock;
    n->handles++;
}

/* ---- ps_vfs operations ---- */

/* Hold mu. */
static int open_locked(fault_vfs *fv, const char *path, bool create, bool excl, node **out)
{
    lookup l;
    int rv = lookup_path(fv, path, &l);
    if (rv != 0) {
        return rv;
    }
    if (l.node != NULL) {
        if (create && excl) {
            rv = EEXIST;
        } else if (l.node->is_dir) {
            rv = EISDIR;
        } else {
            *out = l.node;
        }
        lookup_free(&l);
        return rv;
    }
    if (!create) {
        lookup_free(&l);
        return ENOENT;
    }
    rv = count_point(fv);
    if (rv == 0) {
        node *n = node_new(fv, false);
        cur_set(l.parent, l.name, n);
        log_op(l.parent, OP_CREATE, l.name, NULL, n);
        *out = n;
    }
    lookup_free(&l);
    return rv;
}

static int fv_open(ps_vfs_ctx *ctx, const char *path, u32 flags, ps_file *out)
{
    fault_vfs *fv = fv_of(ctx);
    pthread_mutex_lock(&fv->mu);
    int rv = EIO;
    if (!fv->crashed) {
        node *n = NULL;
        const bool create = (flags & PS_VFS_CREATE) != 0;
        rv = open_locked(fv, path, create, create && (flags & PS_VFS_EXCL) != 0, &n);
        if (rv == 0) {
            fill_handle(fv, out, n, false);
        }
    }
    pthread_mutex_unlock(&fv->mu);
    return rv;
}

static void fv_close(ps_vfs_ctx *ctx, ps_file *f)
{
    if (f == NULL) {
        return;
    }
    fault_vfs *fv = fv_of(ctx);
    pthread_mutex_lock(&fv->mu);
    node *n = NULL;
    if (get_handle(fv, f, &n) == 0) {
        n->handles--;
        if (ff(f)->lock) {
            n->locked = false;
        }
        if (n->handles == 0 && n->links == 0) {
            gc(fv);
        }
    }
    memset(f, 0, sizeof(*f));
    pthread_mutex_unlock(&fv->mu);
}

static int fv_pread(ps_vfs_ctx *ctx, ps_file *f, void *buf, usize len, u64 off, usize *got)
{
    fault_vfs *fv = fv_of(ctx);
    pthread_mutex_lock(&fv->mu);
    node *n = NULL;
    int rv = fv->crashed ? EIO : get_handle(fv, f, &n);
    if (rv == 0) {
        *got = image_read(&n->cur, off, buf, len);
    }
    pthread_mutex_unlock(&fv->mu);
    return rv;
}

static int fv_pwrite(ps_vfs_ctx *ctx, ps_file *f, const void *buf, usize len, u64 off)
{
    fault_vfs *fv = fv_of(ctx);
    pthread_mutex_lock(&fv->mu);
    node *n = NULL;
    int rv = fv->crashed ? EIO : get_handle(fv, f, &n);
    if (rv == 0) {
        rv = count_point(fv);
    }
    if (rv == 0 && (off > MAX_FILE_SIZE || (u64) len > MAX_FILE_SIZE - off)) {
        rv = EFBIG;
    }
    if (rv == 0 && len > 0) {
        const u64 end = off + len;
        rv = space_check(fv, n, end > n->cur.size ? end : n->cur.size);
        if (rv == 0) {
            image_write(&n->cur, off, buf, len);
            log_change(n, CH_WRITE, off, buf, len);
        }
    }
    pthread_mutex_unlock(&fv->mu);
    return rv;
}

static int fv_fsync(ps_vfs_ctx *ctx, ps_file *f)
{
    fault_vfs *fv = fv_of(ctx);
    pthread_mutex_lock(&fv->mu);
    node *n = NULL;
    int rv = fv->crashed ? EIO : get_handle(fv, f, &n);
    if (rv == 0) {
        rv = count_point(fv);
    }
    if (rv == 0) {
        /* Replay changes to leave untouched blocks in place. */
        for (usize i = 0; i < n->nchanges; i++) {
            change_apply(&n->dur, &n->changes[i], n->changes[i].len);
        }
        changes_clear(n);
    }
    pthread_mutex_unlock(&fv->mu);
    return rv;
}

static int fv_size(ps_vfs_ctx *ctx, ps_file *f, u64 *out)
{
    fault_vfs *fv = fv_of(ctx);
    pthread_mutex_lock(&fv->mu);
    node *n = NULL;
    int rv = fv->crashed ? EIO : get_handle(fv, f, &n);
    if (rv == 0) {
        *out = n->cur.size;
    }
    pthread_mutex_unlock(&fv->mu);
    return rv;
}

/* Preallocation shares the truncate path with grow_only set. */
static int set_size(ps_vfs_ctx *ctx, ps_file *f, u64 size, bool grow_only)
{
    fault_vfs *fv = fv_of(ctx);
    pthread_mutex_lock(&fv->mu);
    node *n = NULL;
    int rv = fv->crashed ? EIO : get_handle(fv, f, &n);
    if (rv == 0) {
        rv = count_point(fv);
    }
    if (rv == 0 && size > MAX_FILE_SIZE) {
        rv = EFBIG;
    }
    if (rv == 0 && !(grow_only && size <= n->cur.size)) {
        rv = space_check(fv, n, size);
        if (rv == 0) {
            image_set_size(&n->cur, size);
            log_change(n, CH_SETSIZE, size, NULL, 0);
        }
    }
    pthread_mutex_unlock(&fv->mu);
    return rv;
}

static int fv_truncate(ps_vfs_ctx *ctx, ps_file *f, u64 size)
{
    return set_size(ctx, f, size, false);
}

static int fv_preallocate(ps_vfs_ctx *ctx, ps_file *f, u64 size)
{
    return set_size(ctx, f, size, true);
}

static int fv_unlink(ps_vfs_ctx *ctx, const char *path)
{
    fault_vfs *fv = fv_of(ctx);
    pthread_mutex_lock(&fv->mu);
    int rv = fv->crashed ? EIO : count_point(fv);
    lookup l = {0};
    if (rv == 0) {
        rv = lookup_path(fv, path, &l);
    }
    if (rv == 0) {
        if (l.node == NULL) {
            rv = ENOENT;
        } else if (l.node->is_dir) {
            rv = EISDIR;
        } else {
            cur_remove(l.parent, l.name);
            log_op(l.parent, OP_UNLINK, l.name, NULL, NULL);
        }
    }
    lookup_free(&l);
    pthread_mutex_unlock(&fv->mu);
    return rv;
}

static int fv_rename(ps_vfs_ctx *ctx, const char *from, const char *to)
{
    fault_vfs *fv = fv_of(ctx);
    pthread_mutex_lock(&fv->mu);
    int rv = fv->crashed ? EIO : count_point(fv);
    lookup a = {0};
    lookup b = {0};
    if (rv == 0) {
        rv = lookup_path(fv, from, &a);
    }
    if (rv == 0) {
        rv = lookup_path(fv, to, &b);
    }
    if (rv == 0) {
        if (a.node == NULL) {
            rv = ENOENT;
        } else if (a.parent == NULL || b.parent == NULL) {
            rv = EBUSY; /* the root */
        } else if (a.parent != b.parent) {
            rv = EXDEV;
        } else if (strcmp(a.name, b.name) == 0) {
            rv = 0;
        } else if (b.node != NULL && b.node->is_dir) {
            rv = EISDIR;
        } else if (b.node != NULL && a.node->is_dir) {
            rv = ENOTDIR;
        } else {
            node *n = a.node;
            cur_remove(a.parent, a.name);
            cur_set(a.parent, b.name, n);
            log_op(a.parent, OP_RENAME, a.name, b.name, NULL);
        }
    }
    lookup_free(&a);
    lookup_free(&b);
    pthread_mutex_unlock(&fv->mu);
    return rv;
}

static int fv_mkdir(ps_vfs_ctx *ctx, const char *path)
{
    fault_vfs *fv = fv_of(ctx);
    pthread_mutex_lock(&fv->mu);
    int rv = fv->crashed ? EIO : count_point(fv);
    lookup l = {0};
    if (rv == 0) {
        rv = lookup_path(fv, path, &l);
    }
    if (rv == 0) {
        if (l.node != NULL) {
            rv = EEXIST;
        } else {
            node *n = node_new(fv, true);
            cur_set(l.parent, l.name, n);
            log_op(l.parent, OP_CREATE, l.name, NULL, n);
        }
    }
    lookup_free(&l);
    pthread_mutex_unlock(&fv->mu);
    return rv;
}

static int fv_fsync_dir(ps_vfs_ctx *ctx, const char *path)
{
    fault_vfs *fv = fv_of(ctx);
    pthread_mutex_lock(&fv->mu);
    int rv = fv->crashed ? EIO : count_point(fv);
    lookup l = {0};
    if (rv == 0) {
        rv = lookup_path(fv, path, &l);
    }
    if (rv == 0) {
        if (l.node == NULL) {
            rv = ENOENT;
        } else if (!l.node->is_dir) {
            rv = ENOTDIR;
        } else {
            node *d = l.node;
            ents_free(&d->ents_dur);
            ents_copy(&d->ents_dur, &d->ents_cur);
            ops_clear(d);
            gc(fv);
        }
    }
    lookup_free(&l);
    pthread_mutex_unlock(&fv->mu);
    return rv;
}

static int fv_list(ps_vfs_ctx *ctx, const char *dir, ps_vfs_list_fn fn, ps_vfs_list_ctx *arg)
{
    fault_vfs *fv = fv_of(ctx);
    pthread_mutex_lock(&fv->mu);
    int rv = fv->crashed ? EIO : 0;
    lookup l = {0};
    if (rv == 0) {
        rv = lookup_path(fv, dir, &l);
    }
    char **names = NULL;
    usize n = 0;
    if (rv == 0) {
        if (l.node == NULL) {
            rv = ENOENT;
        } else if (!l.node->is_dir) {
            rv = ENOTDIR;
        } else {
            n = l.node->ents_cur.n;
            names = xmalloc(n * sizeof(*names));
            for (usize i = 0; i < n; i++) {
                names[i] = xstrdup(l.node->ents_cur.v[i].name);
            }
        }
    }
    lookup_free(&l);
    pthread_mutex_unlock(&fv->mu);

    /* A callback may call back into the VFS. */
    for (usize i = 0; i < n && rv == 0; i++) {
        rv = fn(arg, names[i]);
    }
    for (usize i = 0; i < n; i++) {
        free(names[i]);
    }
    free(names);
    return rv;
}

static int fv_lock(ps_vfs_ctx *ctx, const char *path, ps_file *out)
{
    fault_vfs *fv = fv_of(ctx);
    pthread_mutex_lock(&fv->mu);
    int rv = EIO;
    if (!fv->crashed) {
        node *n = NULL;
        rv = open_locked(fv, path, true, false, &n);
        if (rv == 0 && n->locked) {
            rv = EBUSY;
        }
        if (rv == 0) {
            n->locked = true;
            fill_handle(fv, out, n, true);
        }
    }
    pthread_mutex_unlock(&fv->mu);
    return rv;
}

/* ---- control interface ---- */

fault_vfs *fault_vfs_create(u64 seed)
{
    ps_vfs_ctx *ctx = xcalloc(sizeof(*ctx));
    fault_vfs *fv = &ctx->fv;
    if (pthread_mutex_init(&fv->mu, NULL) != 0) {
        fprintf(stderr, "fault_vfs: pthread_mutex_init failed\n");
        abort();
    }
    rng_seed(&fv->rng, seed);
    fv->space_limit = UINT64_MAX;
    fv->epoch = 1;
    fv->root = node_new(fv, true);
    fv->vfs = (ps_vfs) {
        .ctx = ctx,
        .open = fv_open,
        .close = fv_close,
        .pread = fv_pread,
        .pwrite = fv_pwrite,
        .fsync = fv_fsync,
        .size = fv_size,
        .truncate = fv_truncate,
        .preallocate = fv_preallocate,
        .unlink = fv_unlink,
        .rename = fv_rename,
        .mkdir = fv_mkdir,
        .fsync_dir = fv_fsync_dir,
        .list = fv_list,
        .lock = fv_lock,
    };
    return fv;
}

void fault_vfs_destroy(fault_vfs *fv)
{
    if (fv == NULL) {
        return;
    }
    node *n = fv->nodes;
    while (n != NULL) {
        node *next = n->next;
        node_free(n);
        n = next;
    }
    pthread_mutex_destroy(&fv->mu);
    free(fv->vfs.ctx);
}

const ps_vfs *fault_vfs_get(fault_vfs *fv)
{
    return &fv->vfs;
}

void fault_vfs_mkdir_durable(fault_vfs *fv, const char *path)
{
    pthread_mutex_lock(&fv->mu);
    lookup l;
    const int rv = lookup_path(fv, path, &l);
    if (rv != 0 || l.parent == NULL || (l.node != NULL && !l.node->is_dir)) {
        fprintf(stderr, "fault_vfs_mkdir_durable(%s): cannot create (error %d)\n", path, rv);
        abort();
    }
    node *n = l.node;
    if (n == NULL) {
        n = node_new(fv, true);
        cur_set(l.parent, l.name, n);
    }
    ents_set(&l.parent->ents_dur, l.name, n);
    lookup_free(&l);
    pthread_mutex_unlock(&fv->mu);
}

u64 fault_vfs_points(fault_vfs *fv)
{
    pthread_mutex_lock(&fv->mu);
    const u64 p = fv->points;
    pthread_mutex_unlock(&fv->mu);
    return p;
}

void fault_vfs_crash_at(fault_vfs *fv, u64 point)
{
    pthread_mutex_lock(&fv->mu);
    fv->crash_at = point;
    pthread_mutex_unlock(&fv->mu);
}

bool fault_vfs_crashed(fault_vfs *fv)
{
    pthread_mutex_lock(&fv->mu);
    const bool c = fv->crashed;
    pthread_mutex_unlock(&fv->mu);
    return c;
}

void fault_vfs_fail_at(fault_vfs *fv, u64 point, int err)
{
    pthread_mutex_lock(&fv->mu);
    fv->fail_at = point;
    fv->fail_err = err;
    pthread_mutex_unlock(&fv->mu);
}

void fault_vfs_enospc_after(fault_vfs *fv, u64 bytes)
{
    pthread_mutex_lock(&fv->mu);
    fv->space_limit = bytes;
    pthread_mutex_unlock(&fv->mu);
}

/* Promote any surviving unsynced changes into the durable image first. */
static void reboot_file(fault_vfs *fv, node *n, fault_mode mode)
{
    if (mode == FAULT_RANDOM_UNSYNCED) {
        for (usize i = 0; i < n->nchanges; i++) {
            const change *c = &n->changes[i];
            const u64 r = rng_below(&fv->rng, 3);
            if (r == 0) {
                change_apply(&n->dur, c, c->len);
            } else if (r == 2 && c->kind == CH_WRITE) {
                const u64 len = rng_below(&fv->rng, c->len / TEAR_UNIT + 1) * TEAR_UNIT;
                change_apply(&n->dur, c, len);
            }
        }
    }
    changes_clear(n);
    image_free(&n->cur);
    image_copy(&n->cur, &n->dur);
}

/* Replay directory operations in order so a rename sees any surviving create. */
static void reboot_dir(fault_vfs *fv, node *d, fault_mode mode)
{
    if (mode == FAULT_RANDOM_UNSYNCED) {
        entries *e = &d->ents_dur;
        for (usize i = 0; i < d->nops; i++) {
            const dirop *op = &d->ops[i];
            if (rng_below(&fv->rng, 2) == 0) {
                continue;
            }
            if (op->kind == OP_CREATE) {
                ents_set(e, op->name, op->node);
                continue;
            }
            const isize j = ents_find(e, op->name);
            if (j < 0) {
                continue; /* The create may have been dropped. */
            }
            if (op->kind == OP_UNLINK) {
                ents_remove_at(e, (usize) j);
            } else {
                node *n = e->v[j].node;
                ents_remove_at(e, (usize) j);
                ents_set(e, op->to, n);
            }
        }
    }
    ops_clear(d);
    ents_free(&d->ents_cur);
    ents_copy(&d->ents_cur, &d->ents_dur);
}

void fault_vfs_reboot(fault_vfs *fv, fault_mode mode)
{
    pthread_mutex_lock(&fv->mu);
    /* Creation order keeps the random choices deterministic for a seed. */
    for (node *n = fv->nodes; n != NULL; n = n->next) {
        if (n->is_dir) {
            reboot_dir(fv, n, mode);
        } else {
            reboot_file(fv, n, mode);
        }
        n->handles = 0;
        n->locked = false;
    }
    fv->epoch++;
    fv->crashed = false;
    fv->crash_at = 0;
    fv->fail_at = 0;
    gc(fv);
    for (node *n = fv->nodes; n != NULL; n = n->next) {
        n->links = 0;
    }
    for (node *d = fv->nodes; d != NULL; d = d->next) {
        for (usize i = 0; d->is_dir && i < d->ents_cur.n; i++) {
            d->ents_cur.v[i].node->links++;
        }
    }
    pthread_mutex_unlock(&fv->mu);
}

int fault_vfs_corrupt(fault_vfs *fv, const char *path, u64 off, u8 xor_mask)
{
    pthread_mutex_lock(&fv->mu);
    lookup l;
    int rv = lookup_path(fv, path, &l);
    if (rv == 0) {
        node *n = l.node;
        if (n == NULL) {
            rv = ENOENT;
        } else if (n->is_dir) {
            rv = EISDIR;
        } else if (off >= n->dur.size) {
            rv = EINVAL;
        } else {
            image_block(&n->dur, off / BLOCK_SIZE, true)[off % BLOCK_SIZE] ^= xor_mask;
            if (off < n->cur.size) {
                image_block(&n->cur, off / BLOCK_SIZE, true)[off % BLOCK_SIZE] ^= xor_mask;
            }
        }
    }
    lookup_free(&l);
    pthread_mutex_unlock(&fv->mu);
    return rv;
}
