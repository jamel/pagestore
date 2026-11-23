/*
 * Radix array nodes. Integers use host byte order, which PageStore
 * requires to be little-endian.
 *
 * Leaf ("RXLF") and inner ("RXIN") node.
 *
 * Offset   +0..3                  +4..7
 *       +---------------------------------------------+
 *  0x00 | ps_page header (24 bytes)                   |
 *       +----------------------+----------------------+
 *  0x18 | level                | width                |
 *       +----------------------+----------------------+
 *  0x20 | base                                        |
 *       +---------------------------------------------+
 *  0x28 | slots, to the end of the page               |
 *       +---------------------------------------------+
 *
 * level is 0 for leaves and counts up toward the root. base is the first
 * index the node covers. A leaf's slots hold width-byte values. An inner
 * node's slots hold u32 child page IDs, 0 for a missing child, and its
 * width is 4.
 *
 * A node at level L covers span(L) = leaf_cap * fanout^L indexes. Child i
 * of an inner node at base b covers [b + i * span(L-1), b + (i+1) * span(L-1)).
 * The root always has base 0, so an index's path depends only on the
 * index and the height. Spans that would pass 2^64 saturate at UINT64_MAX.
 *
 * Implementation notes:
 *
 * - Writing a page keeps its ID, so a parent changes only when it gains or
 *   loses a child.
 * - A trim leaves values below the bound in a surviving leaf. The tree
 *   never shrinks in height, so trimmed subtrees leave zero slots behind.
 */

#include "radix.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- page layout ---------------------------------------------------------- */

#define LEAF_MAGIC 0x464C5852u /* "RXLF" little-endian */
#define INNER_MAGIC 0x4E495852u /* "RXIN" */

#define NODE_LEVEL 0x18
#define NODE_WIDTH 0x1C
#define NODE_BASE 0x20
#define NODE_SLOTS 0x28

#define CHILD_WIDTH 4u

PS_STATIC_ASSERT(NODE_LEVEL == PS_PAGE_HEADER_SIZE, "node fields follow the page header");

static u32 get32(const u8 *p)
{
    u32 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void put32(u8 *p, u32 v)
{
    memcpy(p, &v, sizeof(v));
}

static u64 get64(const u8 *p)
{
    u64 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void put64(u8 *p, u64 v)
{
    memcpy(p, &v, sizeof(v));
}

static u64 slot_get(const u8 *n, u32 width, u64 i)
{
    const u8 *p = n + NODE_SLOTS + i * width;
    return width == 8 ? get64(p) : get32(p);
}

static void slot_put(u8 *n, u32 width, u64 i, u64 v)
{
    u8 *p = n + NODE_SLOTS + i * width;
    if (width == 8) {
        put64(p, v);
    } else {
        put32(p, (u32) v);
    }
}

/* An inner node's child i, PS_PAGE_ID_NONE if it is missing. */
static ps_page_id child_get(const u8 *n, u64 i)
{
    return ps_page_id_from((u32) slot_get(n, CHILD_WIDTH, i));
}

static void child_put(u8 *n, u64 i, ps_page_id child)
{
    slot_put(n, CHILD_WIDTH, i, child.raw);
}

/* ---- geometry ------------------------------------------------------------- */

/* Indexes covered by a node at level, saturating at UINT64_MAX. */
static u64 span(const rx_array *a, u32 level)
{
    u64 n = a->leaf_cap;
    for (u32 i = 0; i < level; i++) {
        if (n > UINT64_MAX / a->fanout) {
            return UINT64_MAX;
        }
        n *= a->fanout;
    }
    return n;
}

/* Indexes covered by the whole tree. */
static u64 tree_span(const rx_array *a)
{
    return a->height == 0 ? 0 : span(a, a->height - 1);
}

/* The child of a node at base whose range holds from, or 0 below base. */
static u64 first_child(u64 base, u64 child_span, u64 from)
{
    return from > base ? (from - base) / child_span : 0;
}

int rx_init(rx_array *a, u32 page_size, u32 width)
{
    if ((width != 4 && width != 8) || page_size < NODE_SLOTS + 2 * 8) {
        return EINVAL;
    }
    a->root = PS_PAGE_ID_NONE;
    a->height = 0;
    a->width = width;
    a->leaf_cap = (page_size - NODE_SLOTS) / width;
    a->fanout = (page_size - NODE_SLOTS) / CHILD_WIDTH;
    return 0;
}

/* ---- node access ---------------------------------------------------------- */

/* Reads come from the current operation, or from snap when it is set. */
typedef struct src {
    ps_store *store;
    ps_snapshot *snap;
} src;

static int src_read(const src *r, ps_page_id id, const u8 **out)
{
    const ps_page *p;
    const int rv = r->snap != NULL ? ps_snapshot_read(r->snap, id, &p) : ps_read(r->store, id, &p);
    if (rv == 0) {
        *out = (const u8 *) p;
    }
    return rv;
}

/* Snapshot reads unpin. Operation pins last until commit or abort. */
static void src_done(const src *r, ps_page_id id)
{
    if (r->snap != NULL) {
        ps_snapshot_unpin(r->snap, id);
    }
}

static bool node_ok(const rx_array *a, const u8 *n, u32 level, u64 base)
{
    const u32 magic = level == 0 ? LEAF_MAGIC : INNER_MAGIC;
    const u32 width = level == 0 ? a->width : CHILD_WIDTH;
    return get32(n) == magic && get32(n + NODE_LEVEL) == level && get32(n + NODE_WIDTH) == width &&
        get64(n + NODE_BASE) == base;
}

/* Reads a node and checks that it is the one expected at level and base. */
static int read_node(
    const rx_array *a,
    const src *r,
    ps_page_id id,
    u32 level,
    u64 base,
    const u8 **out)
{
    const u8 *n;
    const int rv = src_read(r, id, &n);
    if (rv != 0) {
        return rv;
    }
    if (!node_ok(a, n, level, base)) {
        src_done(r, id);
        return EBADMSG;
    }
    *out = n;
    return 0;
}

static int new_node(const rx_array *a, ps_store *s, u32 level, u64 base, ps_page_id *id, u8 **out)
{
    ps_page *p;
    const int rv = ps_alloc(s, id, &p);
    if (rv != 0) {
        return rv;
    }
    u8 *n = (u8 *) p;
    p->magic = level == 0 ? LEAF_MAGIC : INNER_MAGIC;
    put32(n + NODE_LEVEL, level);
    put32(n + NODE_WIDTH, level == 0 ? a->width : CHILD_WIDTH);
    put64(n + NODE_BASE, base);
    *out = n;
    return 0;
}

/* ---- lookup and change ---------------------------------------------------- */

static int lookup(const rx_array *a, const src *r, u64 index, u64 *value)
{
    if (index >= tree_span(a)) {
        return ENOENT;
    }
    ps_page_id id = a->root;
    u64 base = 0;
    for (u32 level = a->height - 1;; level--) {
        const u8 *n;
        const int rv = read_node(a, r, id, level, base, &n);
        if (rv != 0) {
            return rv;
        }
        if (level == 0) {
            *value = slot_get(n, a->width, index - base);
            src_done(r, id);
            return 0;
        }
        const u64 cs = span(a, level - 1);
        const u64 i = (index - base) / cs;
        const ps_page_id child = child_get(n, i);
        src_done(r, id);
        if (ps_page_id_is_none(child)) {
            return ENOENT;
        }
        id = child;
        base += i * cs;
    }
}

int rx_get_op(const rx_array *a, ps_store *s, u64 index, u64 *value)
{
    const src r = {s, NULL};
    return lookup(a, &r, index, value);
}

int rx_get(const rx_array *a, ps_snapshot *snap, u64 index, u64 *value)
{
    const src r = {NULL, snap};
    return lookup(a, &r, index, value);
}

/* Adds levels until the tree covers index. An empty tree starts at the needed height. */
static int grow(rx_array *a, ps_store *s, u64 index)
{
    u8 *n;
    if (ps_page_id_is_none(a->root)) {
        u32 h = 1;
        while (index >= span(a, h - 1)) {
            h++;
        }
        if (h > RX_MAX_HEIGHT) {
            return EOVERFLOW;
        }
        const int rv = new_node(a, s, h - 1, 0, &a->root, &n);
        if (rv == 0) {
            a->height = h;
        }
        return rv;
    }
    while (index >= tree_span(a)) {
        if (a->height == RX_MAX_HEIGHT) {
            return EOVERFLOW;
        }
        ps_page_id id;
        const int rv = new_node(a, s, a->height, 0, &id, &n);
        if (rv != 0) {
            return rv;
        }
        child_put(n, 0, a->root);
        a->root = id;
        a->height++;
    }
    return 0;
}

int rx_set(rx_array *a, ps_store *s, u64 index, u64 value)
{
    if ((a->width == 4 && value > UINT32_MAX) || index == UINT64_MAX) {
        return EINVAL;
    }
    int rv = grow(a, s, index);
    if (rv != 0) {
        return rv;
    }
    const src r = {s, NULL};
    ps_page_id id = a->root;
    u64 base = 0;
    for (u32 level = a->height - 1; level > 0; level--) {
        const u8 *n;
        rv = read_node(a, &r, id, level, base, &n);
        if (rv != 0) {
            return rv;
        }
        const u64 cs = span(a, level - 1);
        const u64 i = (index - base) / cs;
        ps_page_id child = child_get(n, i);
        if (ps_page_id_is_none(child)) {
            ps_page *w;
            rv = ps_write(s, id, &w);
            u8 *c;
            if (rv == 0) {
                rv = new_node(a, s, level - 1, base + i * cs, &child, &c);
            }
            if (rv != 0) {
                return rv;
            }
            child_put((u8 *) w, i, child);
        }
        id = child;
        base += i * cs;
    }
    ps_page *leaf;
    rv = ps_write(s, id, &leaf);
    if (rv != 0) {
        return rv;
    }
    if (!node_ok(a, (const u8 *) leaf, 0, base)) {
        return EBADMSG;
    }
    slot_put((u8 *) leaf, a->width, index - base, value);
    return 0;
}

/* ---- trimming ------------------------------------------------------------- */

/* Frees id and everything below it. Children below from are already gone. */
static int free_tree(const rx_array *a, ps_store *s, ps_page_id id, u32 level, u64 base, u64 from)
{
    if (level > 0) {
        const src r = {s, NULL};
        const u8 *n;
        int rv = read_node(a, &r, id, level, base, &n);
        if (rv != 0) {
            return rv;
        }
        const u64 cs = span(a, level - 1);
        for (u64 i = first_child(base, cs, from); i < a->fanout; i++) {
            const ps_page_id child = child_get(n, i);
            if (!ps_page_id_is_none(child)) {
                rv = free_tree(a, s, child, level - 1, base + i * cs, from);
                if (rv != 0) {
                    return rv;
                }
            }
        }
    }
    return ps_free(s, id);
}

/* Trims inside node id, whose range holds to strictly after its base. */
static int trim_node(
    const rx_array *a,
    ps_store *s,
    ps_page_id id,
    u32 level,
    u64 base,
    u64 from,
    u64 to)
{
    if (level == 0) {
        return 0;
    }
    const src r = {s, NULL};
    const u8 *n;
    int rv = read_node(a, &r, id, level, base, &n);
    if (rv != 0) {
        return rv;
    }
    const u64 cs = span(a, level - 1);
    const u64 last = (to - base) / cs; /* the child holding to */
    u8 *w = NULL;
    for (u64 i = first_child(base, cs, from); i < last; i++) {
        const ps_page_id child = child_get(n, i);
        if (ps_page_id_is_none(child)) {
            continue;
        }
        rv = free_tree(a, s, child, level - 1, base + i * cs, from);
        if (rv == 0 && w == NULL) {
            ps_page *p;
            rv = ps_write(s, id, &p);
            w = (u8 *) p;
            n = w;
        }
        if (rv != 0) {
            return rv;
        }
        child_put(w, i, PS_PAGE_ID_NONE);
    }
    if ((to - base) % cs == 0) {
        return 0; /* child last starts at to, so all of it stays */
    }
    const ps_page_id child = child_get(n, last);
    return ps_page_id_is_none(child) ?
        0 :
        trim_node(a, s, child, level - 1, base + last * cs, from, to);
}

int rx_trim(rx_array *a, ps_store *s, u64 from, u64 to)
{
    if (ps_page_id_is_none(a->root) || to <= from) {
        return 0;
    }
    if (to >= tree_span(a)) {
        const int rv = free_tree(a, s, a->root, a->height - 1, 0, from);
        if (rv == 0) {
            a->root = PS_PAGE_ID_NONE;
            a->height = 0;
        }
        return rv;
    }
    return trim_node(a, s, a->root, a->height - 1, 0, from, to);
}

/* ---- structure check ------------------------------------------------------ */

typedef struct check_state {
    const rx_array *a;
    ps_snapshot *snap;
    u64 lo;
    u64 hi;
    u64 need; /* the next leaf number that [lo, hi) requires */
    rx_stats *st;
    char *msg;
    usize msg_cap;
} check_state;

static int check_fail(check_state *c, ps_page_id id, const char *fmt, ...) PS_PRINTF(3, 4);

static int check_fail(check_state *c, ps_page_id id, const char *fmt, ...)
{
    if (c->msg_cap == 0) {
        return EBADMSG;
    }
    const int n = snprintf(c->msg, c->msg_cap, "page %u: ", id.raw);
    if (n >= 0 && (usize) n < c->msg_cap) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->msg + n, c->msg_cap - (usize) n, fmt, ap);
        va_end(ap);
    }
    return EBADMSG;
}

static int check_leaf(check_state *c, ps_page_id id, u64 base)
{
    c->st->leaves++;
    if (c->lo >= c->hi) {
        return 0;
    }
    const u64 k = base / c->a->leaf_cap;
    if (k != c->need) {
        return check_fail(
            c,
            id,
            "leaf for indexes from %llu is missing",
            (unsigned long long) (c->need * c->a->leaf_cap));
    }
    c->need++;
    return 0;
}

static int check_node(check_state *c, ps_page_id id, u32 level, u64 base)
{
    const rx_array *a = c->a;
    const src r = {NULL, c->snap};
    const u8 *n;
    int rv = src_read(&r, id, &n);
    if (rv != 0) {
        return rv;
    }
    if (!node_ok(a, n, level, base)) {
        src_done(&r, id);
        return check_fail(
            c,
            id,
            "not the level %u node at base %llu",
            level,
            (unsigned long long) base);
    }
    const u64 s = span(a, level);
    const u64 end = s > UINT64_MAX - base ? UINT64_MAX : base + s;
    if (end <= c->lo || base >= c->hi) {
        src_done(&r, id);
        return check_fail(
            c,
            id,
            "covers %llu..%llu, outside the live range %llu..%llu",
            (unsigned long long) base,
            (unsigned long long) end,
            (unsigned long long) c->lo,
            (unsigned long long) c->hi);
    }
    if (level == 0) {
        rv = check_leaf(c, id, base);
        src_done(&r, id);
        return rv;
    }
    c->st->inner++;
    const u64 cs = span(a, level - 1);
    for (u64 i = 0; i < a->fanout && rv == 0; i++) {
        const ps_page_id child = child_get(n, i);
        if (!ps_page_id_is_none(child)) {
            rv = check_node(c, child, level - 1, base + i * cs);
        }
    }
    src_done(&r, id);
    return rv;
}

int rx_check(
    const rx_array *a,
    ps_snapshot *snap,
    u64 lo,
    u64 hi,
    rx_stats *out,
    char *msg,
    usize msg_cap)
{
    *out = (rx_stats) {0};
    check_state c = {
        .a = a,
        .snap = snap,
        .lo = lo,
        .hi = hi,
        .need = lo / a->leaf_cap,
        .st = out,
        .msg = msg,
        .msg_cap = msg_cap,
    };
    if (ps_page_id_is_none(a->root) != (a->height == 0) || a->height > RX_MAX_HEIGHT) {
        return check_fail(&c, a->root, "bad root for height %u", a->height);
    }
    if (!ps_page_id_is_none(a->root)) {
        const int rv = check_node(&c, a->root, a->height - 1, 0);
        if (rv != 0) {
            return rv;
        }
    }
    if (lo < hi && c.need <= (hi - 1) / a->leaf_cap) {
        return check_fail(
            &c,
            a->root,
            "leaf for indexes from %llu is missing",
            (unsigned long long) (c.need * a->leaf_cap));
    }
    return 0;
}
