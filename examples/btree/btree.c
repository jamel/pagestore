/*
 * B+tree nodes are slotted pages. A sorted array of 2-byte slots grows up
 * from the node header, and the cells they point to grow down from the
 * end of the page. Integers use host byte order, which PageStore requires
 * to be little-endian.
 *
 * Meta page, one per tree.
 *
 * Offset   +0..3                  +4..7
 *       +---------------------------------------------+
 *  0x00 | ps_page header (24 bytes), magic "BTMT"     |
 *       +----------------------+----------------------+
 *  0x18 | root page ID         | height               |
 *       +----------------------+----------------------+
 *  0x20 | key count                                   |
 *       +---------------------------------------------+
 *
 * Leaf ("BTLF") and inner ("BTIN") node.
 *
 * Offset   +0..1       +2..3       +4..5       +6..7
 *       +-----------------------------------------------+
 *  0x00 | ps_page header (24 bytes)                     |
 *       +-----------+-----------+-----------------------+
 *  0x18 | count     | level     | link                  |
 *       +-----------+-----------+-----------+-----------+
 *  0x20 | cell_start            | garbage   | last      |
 *       +-----------------------+-----------+-----------+
 *  0x28 | slots: count u16 cell offsets, in key order   |
 *       |   ... free gap ...                            |
 *       | cells, packed against the end of the page     |
 *       +-----------------------------------------------+
 *
 * level is 0 for leaves. A leaf's link is the next leaf, or 0 for the last
 * one; an inner node's link is its leftmost child. garbage counts bytes of
 * removed cells. last is the slot of the latest insert, or 0xFFFF after any
 * other change.
 *
 *     leaf cell:   u16 klen | u16 vlen | key | value
 *     inner cell:  u32 child | u16 klen | key
 *
 * An inner node with n cells has n + 1 children. Cell i's child holds the
 * keys from cell i's key up to, but not including, cell i + 1's key.
 *
 *         [ link=A | "f"->B | "p"->C ]
 *          /           |          \
 *     keys < "f"   "f" <= keys < "p"   keys >= "p"
 *
 * Implementation notes:
 *
 * - An entry with its slot takes at most a quarter of a node, so a node
 *   that overflows by one entry always splits into two halves that fit.
 * - Splits divide by bytes. During an ascending or descending insert run
 *   they split next to the new entry instead, so sorted loads fill pages.
 * - A leaf split passes up the shortest key prefix that separates the halves.
 * - A node under a quarter full after a delete merges with or borrows from
 *   a sibling. If the parent has no room for the new separator, the node
 *   stays underfull.
 * - Abort rolls back every changed page, so failed writes need no undo here.
 */

#include "btree.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- page layout ---------------------------------------------------------- */

#define META_MAGIC 0x544D5442u /* "BTMT" little-endian */
#define LEAF_MAGIC 0x464C5442u /* "BTLF" */
#define INNER_MAGIC 0x4E495442u /* "BTIN" */

#define PAGE_MAGIC 0x00 /* ps_page.magic */

#define META_ROOT 0x18
#define META_HEIGHT 0x1C
#define META_COUNT 0x20

#define NODE_COUNT 0x18
#define NODE_LEVEL 0x1A
#define NODE_LINK 0x1C
#define NODE_CELL_START 0x20
#define NODE_GARBAGE 0x24
#define NODE_LAST 0x26
#define NODE_SLOTS 0x28

#define NO_LAST 0xFFFFu

#define SLOT_SIZE 2u
#define LEAF_CELL_HEADER 4u
#define INNER_CELL_HEADER 6u

PS_STATIC_ASSERT(META_ROOT == PS_PAGE_HEADER_SIZE, "tree fields follow the page header");
PS_STATIC_ASSERT(NODE_COUNT == PS_PAGE_HEADER_SIZE, "node fields follow the page header");

/* ---- small helpers -------------------------------------------------------- */

static u32 get16(const u8 *p)
{
    u16 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static void put16(u8 *p, u32 v)
{
    const u16 x = (u16) v;
    memcpy(p, &x, sizeof(x));
}

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

/* memcpy with a NULL source is undefined even for zero bytes. */
static void copy(u8 *dst, const void *src, u32 n)
{
    if (n > 0) {
        memcpy(dst, src, n);
    }
}

static int key_cmp(const u8 *a, u32 alen, const u8 *b, u32 blen)
{
    const u32 n = alen < blen ? alen : blen;
    const int c = n == 0 ? 0 : memcmp(a, b, n);
    if (c != 0) {
        return c;
    }
    return alen < blen ? -1 : alen > blen ? 1 : 0;
}

/*
 * Length of the shortest prefix of hi that sorts above lo. Requires
 * lo < hi, so the prefix is never longer than hi.
 */
static u32 sep_len(const u8 *lo, u32 lolen, const u8 *hi, u32 hilen)
{
    u32 i = 0;
    while (i < lolen && i < hilen && lo[i] == hi[i]) {
        i++;
    }
    assert(i < hilen);
    return i + 1;
}

/* ---- tree geometry and scratch space ---------------------------------------- */

/* Bytes in a node for slots and cells. */
static u32 capacity(const bt_tree *t)
{
    return t->page_size - NODE_SLOTS;
}

/* Most bytes one entry and its slot may take. */
static u32 max_cost(u32 page_size)
{
    return (page_size - NODE_SLOTS) / 4;
}

/* A node using less than this after a delete is rebalanced. */
static u32 underfull(const bt_tree *t)
{
    return capacity(t) / 4;
}

/*
 * The writer's scratch space holds three buffers:
 *
 *     image  a page-sized copy of the node being split or compacted
 *     cell   the cell about to be inserted, at most max_cost bytes
 *     sep    a separator key on its way to the parent, at most max_key bytes
 */
static u8 *image_buf(bt_tree *t)
{
    return t->scratch;
}

static u8 *cell_buf(bt_tree *t)
{
    return t->scratch + t->page_size;
}

static u8 *sep_buf(bt_tree *t)
{
    return t->scratch + t->page_size + max_cost(t->page_size);
}

/* ---- node access ------------------------------------------------------------ */

static u32 n_count(const u8 *n)
{
    return get16(n + NODE_COUNT);
}

static u32 n_level(const u8 *n)
{
    return get16(n + NODE_LEVEL);
}

static bool n_leaf(const u8 *n)
{
    return n_level(n) == 0;
}

static ps_page_id n_link(const u8 *n)
{
    return ps_page_id_from(get32(n + NODE_LINK));
}

static void n_set_link(u8 *n, ps_page_id link)
{
    put32(n + NODE_LINK, link.raw);
}

static u32 n_cell_start(const u8 *n)
{
    return get32(n + NODE_CELL_START);
}

static u32 n_garbage(const u8 *n)
{
    return get16(n + NODE_GARBAGE);
}

static u32 n_last(const u8 *n)
{
    return get16(n + NODE_LAST);
}

static u32 n_slot(const u8 *n, u32 i)
{
    return get16(n + NODE_SLOTS + SLOT_SIZE * i);
}

static const u8 *n_cell(const u8 *n, u32 i)
{
    return n + n_slot(n, i);
}

/* Free bytes between the slots and the cells. */
static u32 n_gap(const u8 *n)
{
    return n_cell_start(n) - (NODE_SLOTS + SLOT_SIZE * n_count(n));
}

/* Free bytes, counting garbage that compaction can reclaim. */
static u32 n_free(const u8 *n)
{
    return n_gap(n) + n_garbage(n);
}

/* Bytes taken by live cells and their slots. */
static u32 n_used(const bt_tree *t, const u8 *n)
{
    return capacity(t) - n_free(n);
}

static u32 cell_size(bool leaf, const u8 *c)
{
    if (leaf) {
        return LEAF_CELL_HEADER + get16(c) + get16(c + 2);
    }
    return INNER_CELL_HEADER + get16(c + 4);
}

/* The space a cell takes in a node, including its slot. */
static u32 cell_cost(bool leaf, const u8 *c)
{
    return cell_size(leaf, c) + SLOT_SIZE;
}

static const u8 *cell_key(bool leaf, const u8 *c, u32 *klen)
{
    if (leaf) {
        *klen = get16(c);
        return c + LEAF_CELL_HEADER;
    }
    *klen = get16(c + 4);
    return c + INNER_CELL_HEADER;
}

static ps_page_id cell_child(const u8 *c)
{
    return ps_page_id_from(get32(c));
}

static const u8 *n_key(const u8 *n, u32 i, u32 *klen)
{
    return cell_key(n_leaf(n), n_cell(n, i), klen);
}

/* Child ci of an inner node: 0 is the leftmost child, i + 1 is cell i's. */
static ps_page_id n_child(const u8 *n, u32 ci)
{
    return ci == 0 ? n_link(n) : cell_child(n_cell(n, ci - 1));
}

static u32 leaf_cell_build(u8 *out, const void *key, u32 klen, const void *val, u32 vlen)
{
    put16(out, klen);
    put16(out + 2, vlen);
    copy(out + LEAF_CELL_HEADER, key, klen);
    copy(out + LEAF_CELL_HEADER + klen, val, vlen);
    return LEAF_CELL_HEADER + klen + vlen;
}

static u32 inner_cell_build(u8 *out, ps_page_id child, const u8 *key, u32 klen)
{
    put32(out, child.raw);
    put16(out + 4, klen);
    copy(out + INNER_CELL_HEADER, key, klen);
    return INNER_CELL_HEADER + klen;
}

/* First slot whose key is at or after key. */
static u32 node_lower(const u8 *n, const u8 *key, u32 klen, bool *found)
{
    u32 lo = 0;
    u32 hi = n_count(n);
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        u32 mlen;
        const u8 *mkey = n_key(n, mid, &mlen);
        if (key_cmp(mkey, mlen, key, klen) < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    *found = false;
    if (lo < n_count(n)) {
        u32 len;
        const u8 *k = n_key(n, lo, &len);
        *found = key_cmp(k, len, key, klen) == 0;
    }
    return lo;
}

/* The child of an inner node whose range holds key: the count of separators <= key. */
static u32 inner_find(const u8 *n, const u8 *key, u32 klen)
{
    u32 lo = 0;
    u32 hi = n_count(n);
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2;
        u32 mlen;
        const u8 *mkey = n_key(n, mid, &mlen);
        if (key_cmp(mkey, mlen, key, klen) <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* Checks the header fields that every reader relies on. */
static bool node_sane(u32 page_size, const u8 *n, u32 level)
{
    const u32 magic = level == 0 ? LEAF_MAGIC : INNER_MAGIC;
    if (get32(n + PAGE_MAGIC) != magic || n_level(n) != level) {
        return false;
    }
    const u32 start = n_cell_start(n);
    return NODE_SLOTS + SLOT_SIZE * n_count(n) <= start && start <= page_size &&
        n_garbage(n) <= page_size - start;
}

/* ---- node changes ----------------------------------------------------------- */

/* Leaves the store-owned header fields alone. */
static void node_init(u32 page_size, u8 *n, u32 level, ps_page_id link)
{
    put32(n + PAGE_MAGIC, level == 0 ? LEAF_MAGIC : INNER_MAGIC);
    put16(n + NODE_COUNT, 0);
    put16(n + NODE_LEVEL, level);
    n_set_link(n, link);
    put32(n + NODE_CELL_START, page_size);
    put16(n + NODE_GARBAGE, 0);
    put16(n + NODE_LAST, NO_LAST);
}

/* Repacks the cells at the page end so that all free space is in the gap. */
static void node_compact(bt_tree *t, u8 *n)
{
    u8 *img = image_buf(t);
    memcpy(img, n, t->page_size);
    const bool leaf = n_leaf(n);
    u32 end = t->page_size;
    for (u32 i = 0; i < n_count(img); i++) {
        const u8 *c = n_cell(img, i);
        const u32 size = cell_size(leaf, c);
        end -= size;
        memcpy(n + end, c, size);
        put16(n + NODE_SLOTS + SLOT_SIZE * i, end);
    }
    put32(n + NODE_CELL_START, end);
    put16(n + NODE_GARBAGE, 0);
}

/*
 * Inserts a cell at slot i. The caller checks that n_free covers its cost.
 * Compaction overwrites the image buffer, so cell must not point there.
 * Clears the last insert slot. Only the insert path sets it.
 */
static void node_insert(bt_tree *t, u8 *n, u32 i, const u8 *cell, u32 size)
{
    assert(n_free(n) >= size + SLOT_SIZE);
    if (n_gap(n) < size + SLOT_SIZE) {
        node_compact(t, n);
    }
    const u32 count = n_count(n);
    const u32 at = n_cell_start(n) - size;
    memcpy(n + at, cell, size);
    u8 *slot = n + NODE_SLOTS + SLOT_SIZE * i;
    memmove(slot + SLOT_SIZE, slot, SLOT_SIZE * (count - i));
    put16(slot, at);
    put16(n + NODE_COUNT, count + 1);
    put32(n + NODE_CELL_START, at);
    put16(n + NODE_LAST, NO_LAST);
}

/* Adds a cell to a node being rebuilt. Such a node has no garbage, so nothing gets compacted. */
static void node_append(bt_tree *t, u8 *n, const u8 *cell, u32 size)
{
    assert(n_gap(n) >= size + SLOT_SIZE);
    node_insert(t, n, n_count(n), cell, size);
}

static void node_remove(u8 *n, u32 i)
{
    const u32 count = n_count(n);
    const u32 size = cell_size(n_leaf(n), n_cell(n, i));
    u8 *slot = n + NODE_SLOTS + SLOT_SIZE * i;
    memmove(slot, slot + SLOT_SIZE, SLOT_SIZE * (count - i - 1));
    put16(n + NODE_COUNT, count - 1);
    put16(n + NODE_GARBAGE, n_garbage(n) + size);
    put16(n + NODE_LAST, NO_LAST);
}

/* ---- meta page ---------------------------------------------------------------- */

typedef struct tree_meta {
    ps_page_id root;
    u32 height;
    u64 count;
} tree_meta;

static int meta_decode(const ps_page *page, tree_meta *m)
{
    const u8 *p = (const u8 *) page;
    if (get32(p + PAGE_MAGIC) != META_MAGIC) {
        return EBADMSG;
    }
    m->root = ps_page_id_from(get32(p + META_ROOT));
    m->height = get32(p + META_HEIGHT);
    m->count = get64(p + META_COUNT);
    if (ps_page_id_is_none(m->root) || m->height == 0 || m->height > BT_MAX_HEIGHT) {
        return EBADMSG;
    }
    return 0;
}

static void meta_encode(ps_page *page, const tree_meta *m)
{
    u8 *p = (u8 *) page;
    put32(p + PAGE_MAGIC, META_MAGIC);
    put32(p + META_ROOT, m->root.raw);
    put32(p + META_HEIGHT, m->height);
    put64(p + META_COUNT, m->count);
}

/* ---- writer page access --------------------------------------------------------- */

static int meta_load(bt_tree *t, tree_meta *m)
{
    const ps_page *p;
    const int rv = ps_read(t->store, t->meta_id, &p);
    return rv != 0 ? rv : meta_decode(p, m);
}

static int meta_store(bt_tree *t, const tree_meta *m)
{
    ps_page *p;
    const int rv = ps_write(t->store, t->meta_id, &p);
    if (rv == 0) {
        meta_encode(p, m);
    }
    return rv;
}

/* Inside an operation this sees the operation's own changes. */
static int read_node(bt_tree *t, ps_page_id id, u32 level, const u8 **out)
{
    const ps_page *p;
    const int rv = ps_read(t->store, id, &p);
    if (rv != 0) {
        return rv;
    }
    if (!node_sane(t->page_size, (const u8 *) p, level)) {
        return EBADMSG;
    }
    *out = (const u8 *) p;
    return 0;
}

/* Returns the operation's private copy of the node, which is safe to change. */
static int write_node(bt_tree *t, ps_page_id id, u32 level, u8 **out)
{
    ps_page *p;
    const int rv = ps_write(t->store, id, &p);
    if (rv != 0) {
        return rv;
    }
    if (!node_sane(t->page_size, (const u8 *) p, level)) {
        return EBADMSG;
    }
    *out = (u8 *) p;
    return 0;
}

/* The nodes from the root (depth 0) down to the leaf that holds or would hold a key. */
typedef struct tree_path {
    ps_page_id ids[BT_MAX_HEIGHT];
    /* The child taken in an inner node. In the leaf, the key's slot. */
    u32 slots[BT_MAX_HEIGHT];
} tree_path;

static int descend(
    bt_tree *t,
    const tree_meta *m,
    const u8 *key,
    u32 klen,
    tree_path *p,
    bool *found)
{
    ps_page_id id = m->root;
    for (u32 d = 0;; d++) {
        const u32 level = m->height - 1 - d;
        const u8 *n;
        const int rv = read_node(t, id, level, &n);
        if (rv != 0) {
            return rv;
        }
        p->ids[d] = id;
        if (level == 0) {
            p->slots[d] = node_lower(n, key, klen, found);
            return 0;
        }
        const u32 ci = inner_find(n, key, klen);
        p->slots[d] = ci;
        id = n_child(n, ci);
    }
}

/* ---- insert ------------------------------------------------------------------- */

/* Entry j of a node image with the new cell placed at slot i. */
static const u8 *split_entry(const u8 *old, u32 i, const u8 *cell, u32 j)
{
    if (j == i) {
        return cell;
    }
    return n_cell(old, j < i ? j : j - 1);
}

/*
 * Bytes each side would use if the entries of old plus the new cell at
 * slot i were split at k. See split_point.
 */
static void split_sizes(const u8 *old, u32 i, const u8 *cell, u32 k, u32 *left, u32 *right)
{
    const bool leaf = n_leaf(old);
    const u32 total = n_count(old) + 1;
    *left = 0;
    *right = 0;
    for (u32 j = 0; j < total; j++) {
        const u32 cost = cell_cost(leaf, split_entry(old, i, cell, j));
        if (j < k) {
            *left += cost;
        } else if (leaf || j > k) {
            *right += cost;
        }
    }
}

/*
 * Picks where to split the entries of old plus the new cell at slot i.
 * A leaf keeps entries [0, k) and moves the rest to its new sibling. An
 * inner node keeps [0, k), passes entry k up and moves the rest.
 */
static u32 split_point(const bt_tree *t, const u8 *old, u32 i, const u8 *cell)
{
    const bool leaf = n_leaf(old);
    const u32 total = n_count(old) + 1;
    const u32 last = leaf ? total - 1 : total - 2; /* each side keeps an entry */
    const u32 cap = capacity(t);
    const u32 prev = n_last(old);
    u32 k = 0;
    if (prev != NO_LAST && i == prev + 1) {
        k = leaf ? i : i - 1; /* ascending: the new entry starts the right side */
    } else if (prev != NO_LAST && i == prev) {
        k = i + 1; /* descending: the new entry ends the left side */
    }
    u32 left;
    u32 right;
    if (k >= 1 && k <= last) {
        split_sizes(old, i, cell, k, &left, &right);
        if (left <= cap && right <= cap) {
            return k;
        }
    }
    u32 sum = 0;
    for (u32 j = 0; j < total; j++) {
        sum += cell_cost(leaf, split_entry(old, i, cell, j));
    }
    u32 best = 0;
    u32 best_diff = UINT32_MAX;
    left = 0;
    for (k = 1; k <= last; k++) {
        left += cell_cost(leaf, split_entry(old, i, cell, k - 1));
        if (left > cap) {
            break;
        }
        right = sum - left;
        if (!leaf) {
            right -= cell_cost(leaf, split_entry(old, i, cell, k));
        }
        if (right > cap) {
            continue;
        }
        const u32 diff = left > right ? left - right : right - left;
        if (diff < best_diff) {
            best = k;
            best_diff = diff;
        }
    }
    assert(best != 0);
    return best;
}

/*
 * Splits n while inserting the cell buffer at slot i. n keeps the lower
 * entries and a new right sibling takes the rest. Leaves the separator
 * for the parent in the separator buffer.
 */
static int node_split(bt_tree *t, u8 *n, u32 i, ps_page_id *right_id, u32 *seplen)
{
    ps_page_id rid;
    ps_page *rp;
    const int rv = ps_alloc(t->store, &rid, &rp);
    if (rv != 0) {
        return rv;
    }
    u8 *r = (u8 *) rp;
    u8 *old = image_buf(t);
    memcpy(old, n, t->page_size);
    const u8 *cell = cell_buf(t);
    const bool leaf = n_leaf(old);
    const u32 level = n_level(old);
    const u32 total = n_count(old) + 1;
    const u32 k = split_point(t, old, i, cell);

    const u8 *mid = split_entry(old, i, cell, k);
    if (leaf) {
        node_init(t->page_size, n, 0, rid);
        node_init(t->page_size, r, 0, n_link(old));
    } else {
        node_init(t->page_size, n, level, n_link(old));
        node_init(t->page_size, r, level, cell_child(mid));
    }
    for (u32 j = 0; j < total; j++) {
        if (!leaf && j == k) {
            continue;
        }
        const u8 *c = split_entry(old, i, cell, j);
        node_append(t, j < k ? n : r, c, cell_size(leaf, c));
    }
    /* Keep the last insert slot so that a run carries on in the new nodes. */
    if (i < k) {
        put16(n + NODE_LAST, i);
    } else if (leaf || i > k) {
        put16(r + NODE_LAST, leaf ? i - k : i - k - 1);
    }

    u32 hilen;
    const u8 *hi = cell_key(leaf, mid, &hilen);
    *seplen = hilen;
    if (leaf) {
        u32 lolen;
        const u8 *lo = cell_key(leaf, split_entry(old, i, cell, k - 1), &lolen);
        *seplen = sep_len(lo, lolen, hi, hilen);
    }
    copy(sep_buf(t), hi, *seplen);
    *right_id = rid;
    return 0;
}

/* Puts a new root above old_root. The cell buffer holds its one cell. */
static int grow_root(bt_tree *t, tree_meta *m, ps_page_id old_root, u32 level, u32 size)
{
    if (m->height >= BT_MAX_HEIGHT) {
        return EOVERFLOW;
    }
    ps_page_id id;
    ps_page *p;
    const int rv = ps_alloc(t->store, &id, &p);
    if (rv != 0) {
        return rv;
    }
    u8 *n = (u8 *) p;
    node_init(t->page_size, n, level, old_root);
    node_append(t, n, cell_buf(t), size);
    put16(n + NODE_LAST, 0);
    m->root = id;
    m->height++;
    return 0;
}

/*
 * Inserts the cell buffer at slot i of the node at depth d. A full node
 * splits and its separator moves up, possibly all the way to a new root.
 */
static int insert_up(bt_tree *t, tree_meta *m, const tree_path *p, u32 d, u32 i, u32 size)
{
    for (;;) {
        u8 *n;
        int rv = write_node(t, p->ids[d], m->height - 1 - d, &n);
        if (rv != 0) {
            return rv;
        }
        if (n_free(n) >= size + SLOT_SIZE) {
            node_insert(t, n, i, cell_buf(t), size);
            put16(n + NODE_LAST, i);
            return 0;
        }
        ps_page_id right;
        u32 seplen;
        rv = node_split(t, n, i, &right, &seplen);
        if (rv != 0) {
            return rv;
        }
        const u32 parent_level = n_level(n) + 1;
        size = inner_cell_build(cell_buf(t), right, sep_buf(t), seplen);
        if (d == 0) {
            return grow_root(t, m, p->ids[0], parent_level, size);
        }
        d--;
        i = p->slots[d]; /* just after the cell that leads to the split node */
    }
}

int bt_put(bt_tree *t, const void *key, u32 klen, const void *val, u32 vlen)
{
    if ((key == NULL && klen > 0) || (val == NULL && vlen > 0) || klen > t->max_key ||
        vlen > t->max_entry - klen) {
        return EINVAL;
    }
    tree_meta m;
    int rv = meta_load(t, &m);
    if (rv != 0) {
        return rv;
    }
    const tree_meta before = m;
    tree_path p;
    bool found;
    rv = descend(t, &m, key, klen, &p, &found);
    if (rv != 0) {
        return rv;
    }
    const u32 d = m.height - 1;
    u8 *leaf;
    rv = write_node(t, p.ids[d], 0, &leaf);
    if (rv != 0) {
        return rv;
    }
    if (found) {
        node_remove(leaf, p.slots[d]);
    } else {
        m.count++;
    }
    const u32 size = leaf_cell_build(cell_buf(t), key, klen, val, vlen);
    rv = insert_up(t, &m, &p, d, p.slots[d], size);
    if (rv != 0) {
        return rv;
    }
    if (m.count == before.count && ps_page_id_eq(m.root, before.root)) {
        return 0;
    }
    return meta_store(t, &m);
}

/* ---- delete ------------------------------------------------------------------- */

/* True if moving one entry narrows the size gap without overfilling the receiver. */
static bool narrows(u32 donor, u32 receiver, u32 loss, u32 gain, u32 cap)
{
    if (receiver + gain > cap || loss > donor) {
        return false;
    }
    const i64 before = (i64) donor - (i64) receiver;
    const i64 after = (i64) (donor - loss) - (i64) (receiver + gain);
    return (after < 0 ? -after : after) < (before < 0 ? -before : before);
}

/* True if separator si can grow or shrink to newlen bytes. */
static bool sep_fits(const u8 *parent, u32 si, u32 newlen)
{
    u32 oldlen;
    n_key(parent, si, &oldlen);
    return newlen <= oldlen || newlen - oldlen <= n_free(parent);
}

/* Replaces the key of separator si with the separator buffer, keeping its child. */
static void set_separator(bt_tree *t, u8 *parent, u32 si, u32 seplen)
{
    const ps_page_id child = cell_child(n_cell(parent, si));
    node_remove(parent, si);
    const u32 size = inner_cell_build(cell_buf(t), child, sep_buf(t), seplen);
    node_insert(t, parent, si, cell_buf(t), size);
}

/* Moves all of r into l and drops separator si, which led to r. */
static void merge(bt_tree *t, u8 *parent, u32 si, u8 *l, const u8 *r)
{
    const bool leaf = n_leaf(l);
    if (leaf) {
        n_set_link(l, n_link(r));
    } else {
        /* The separator comes down to sit above r's leftmost child. */
        u32 seplen;
        const u8 *sep = n_key(parent, si, &seplen);
        const u32 size = inner_cell_build(cell_buf(t), n_link(r), sep, seplen);
        node_insert(t, l, n_count(l), cell_buf(t), size);
    }
    for (u32 i = 0; i < n_count(r); i++) {
        const u8 *c = n_cell(r, i);
        node_insert(t, l, n_count(l), c, cell_size(leaf, c));
    }
    node_remove(parent, si);
}

/*
 * Moves leaf entries from the fuller of l and r to the other while that
 * narrows the size gap. The new separator is the shortest one between the
 * two halves. Fewer entries move if the parent lacks room for it.
 */
static void borrow_leaf(bt_tree *t, u8 *parent, u32 si, u8 *l, u8 *r)
{
    const u32 cap = capacity(t);
    u32 lu = n_used(t, l);
    u32 ru = n_used(t, r);
    const u32 lc = n_count(l);
    const u32 rc = n_count(r);
    const bool to_left = lu < ru;
    u32 k = 0;
    if (to_left) {
        while (k + 1 < rc) {
            const u32 e = cell_cost(true, n_cell(r, k));
            if (!narrows(ru, lu, e, e, cap)) {
                break;
            }
            ru -= e;
            lu += e;
            k++;
        }
    } else {
        while (k + 1 < lc) {
            const u32 e = cell_cost(true, n_cell(l, lc - 1 - k));
            if (!narrows(lu, ru, e, e, cap)) {
                break;
            }
            lu -= e;
            ru += e;
            k++;
        }
    }
    u32 seplen = 0;
    for (; k > 0; k--) {
        u32 lolen;
        u32 hilen;
        const u8 *lo = to_left ? n_key(r, k - 1, &lolen) : n_key(l, lc - k - 1, &lolen);
        const u8 *hi = to_left ? n_key(r, k, &hilen) : n_key(l, lc - k, &hilen);
        seplen = sep_len(lo, lolen, hi, hilen);
        if (sep_fits(parent, si, seplen)) {
            copy(sep_buf(t), hi, seplen);
            break;
        }
    }
    if (k == 0) {
        return;
    }
    if (to_left) {
        for (u32 j = 0; j < k; j++) {
            const u8 *c = n_cell(r, j);
            node_insert(t, l, n_count(l), c, cell_size(true, c));
        }
        for (u32 j = 0; j < k; j++) {
            node_remove(r, 0);
        }
    } else {
        for (u32 j = 0; j < k; j++) {
            const u8 *c = n_cell(l, lc - 1 - j);
            node_insert(t, r, 0, c, cell_size(true, c));
        }
        for (u32 j = 0; j < k; j++) {
            node_remove(l, n_count(l) - 1);
        }
    }
    set_separator(t, parent, si, seplen);
}

/*
 * Rotates inner entries through the parent from the fuller of l and r to
 * the other. Moving k entries to the left looks like this, with s the
 * old separator and r0..r(k-1) the cells taken from r:
 *
 *     l gains (s -> r.leftmost), r0 .. r(k-2)
 *     the parent's separator becomes r(k-1)'s key
 *     r's leftmost child becomes r(k-1)'s child
 *
 * Moving to the right mirrors this. Fewer entries move if the parent lacks
 * room for the new separator.
 */
static void borrow_inner(bt_tree *t, u8 *parent, u32 si, u8 *l, u8 *r)
{
    const u32 cap = capacity(t);
    u32 lu = n_used(t, l);
    u32 ru = n_used(t, r);
    const u32 lc = n_count(l);
    const u32 rc = n_count(r);
    const bool to_left = lu < ru;
    u32 glen; /* key length of the separator the next step brings down */
    n_key(parent, si, &glen);
    u32 k = 0;
    if (to_left) {
        while (k + 1 < rc) {
            const u32 gain = INNER_CELL_HEADER + glen + SLOT_SIZE;
            const u32 loss = cell_cost(false, n_cell(r, k));
            if (!narrows(ru, lu, loss, gain, cap)) {
                break;
            }
            ru -= loss;
            lu += gain;
            n_key(r, k, &glen);
            k++;
        }
    } else {
        while (k + 1 < lc) {
            const u32 gain = INNER_CELL_HEADER + glen + SLOT_SIZE;
            const u32 loss = cell_cost(false, n_cell(l, lc - 1 - k));
            if (!narrows(lu, ru, loss, gain, cap)) {
                break;
            }
            lu -= loss;
            ru += gain;
            n_key(l, lc - 1 - k, &glen);
            k++;
        }
    }
    u32 seplen = 0;
    for (; k > 0; k--) {
        const u8 *up = to_left ? n_key(r, k - 1, &seplen) : n_key(l, lc - k, &seplen);
        if (sep_fits(parent, si, seplen)) {
            copy(sep_buf(t), up, seplen);
            break;
        }
    }
    if (k == 0) {
        return;
    }
    u32 plen;
    const u8 *pkey = n_key(parent, si, &plen);
    const u32 size = inner_cell_build(cell_buf(t), n_link(r), pkey, plen);
    ps_page_id leftmost;
    if (to_left) {
        node_insert(t, l, n_count(l), cell_buf(t), size);
        for (u32 j = 0; j + 1 < k; j++) {
            const u8 *c = n_cell(r, j);
            node_insert(t, l, n_count(l), c, cell_size(false, c));
        }
        leftmost = cell_child(n_cell(r, k - 1));
        for (u32 j = 0; j < k; j++) {
            node_remove(r, 0);
        }
    } else {
        node_insert(t, r, 0, cell_buf(t), size);
        for (u32 j = 1; j < k; j++) {
            const u8 *c = n_cell(l, lc - j);
            node_insert(t, r, 0, c, cell_size(false, c));
        }
        leftmost = cell_child(n_cell(l, lc - k));
        for (u32 j = 0; j < k; j++) {
            node_remove(l, n_count(l) - 1);
        }
    }
    n_set_link(r, leftmost);
    set_separator(t, parent, si, seplen);
}

/*
 * Fixes an underfull node at depth d, whose parent is at depth d - 1.
 * Sets *merged if two siblings became one, which shrinks the parent.
 */
static int fix_underfull(bt_tree *t, const tree_meta *m, const tree_path *p, u32 d, bool *merged)
{
    *merged = false;
    const u32 level = m->height - 1 - d;
    u8 *parent;
    int rv = write_node(t, p->ids[d - 1], level + 1, &parent);
    if (rv != 0 || n_count(parent) == 0) {
        return rv; /* with no sibling there is nothing to merge with */
    }
    /* Pair the node with its left sibling, or its right one if it has none. */
    const u32 ci = p->slots[d - 1];
    const u32 si = ci > 0 ? ci - 1 : 0; /* the separator between the pair */
    const ps_page_id rid = n_child(parent, si + 1);
    u8 *l;
    u8 *r;
    rv = write_node(t, n_child(parent, si), level, &l);
    if (rv == 0) {
        rv = write_node(t, rid, level, &r);
    }
    if (rv != 0) {
        return rv;
    }
    const bool leaf = level == 0;
    u32 seplen;
    n_key(parent, si, &seplen);
    u32 joined = n_used(t, l) + n_used(t, r);
    if (!leaf) {
        joined += INNER_CELL_HEADER + seplen + SLOT_SIZE;
    }
    if (joined > capacity(t)) {
        if (leaf) {
            borrow_leaf(t, parent, si, l, r);
        } else {
            borrow_inner(t, parent, si, l, r);
        }
        return 0;
    }
    merge(t, parent, si, l, r);
    *merged = true;
    return ps_free(t->store, rid);
}

/* Replaces a root that has a single child with that child. */
static int shrink_root(bt_tree *t, tree_meta *m)
{
    while (m->height > 1) {
        const u8 *root;
        int rv = read_node(t, m->root, m->height - 1, &root);
        if (rv != 0 || n_count(root) > 0) {
            return rv;
        }
        const ps_page_id child = n_link(root);
        rv = ps_free(t->store, m->root);
        if (rv != 0) {
            return rv;
        }
        m->root = child;
        m->height--;
    }
    return 0;
}

int bt_delete(bt_tree *t, const void *key, u32 klen)
{
    if (key == NULL && klen > 0) {
        return EINVAL;
    }
    tree_meta m;
    int rv = meta_load(t, &m);
    if (rv != 0) {
        return rv;
    }
    tree_path p;
    bool found;
    rv = descend(t, &m, key, klen, &p, &found);
    if (rv != 0) {
        return rv;
    }
    if (!found) {
        return ENOENT;
    }
    u8 *leaf;
    rv = write_node(t, p.ids[m.height - 1], 0, &leaf);
    if (rv != 0) {
        return rv;
    }
    node_remove(leaf, p.slots[m.height - 1]);
    m.count--;
    for (u32 d = m.height - 1; d > 0; d--) {
        const u8 *n;
        rv = read_node(t, p.ids[d], m.height - 1 - d, &n);
        if (rv != 0) {
            return rv;
        }
        if (n_used(t, n) >= underfull(t)) {
            break;
        }
        bool merged;
        rv = fix_underfull(t, &m, &p, d, &merged);
        if (rv != 0) {
            return rv;
        }
        if (!merged) {
            break;
        }
    }
    rv = shrink_root(t, &m);
    if (rv != 0) {
        return rv;
    }
    return meta_store(t, &m);
}

/* ---- create and open -------------------------------------------------------------- */

int bt_create(ps_store *s, ps_page_id *meta_id)
{
    ps_page_id mid;
    ps_page_id rid;
    ps_page *mp;
    ps_page *rp;
    int rv = ps_alloc(s, &mid, &mp);
    if (rv == 0) {
        rv = ps_alloc(s, &rid, &rp);
    }
    if (rv != 0) {
        return rv;
    }
    node_init(ps_page_size(s), (u8 *) rp, 0, PS_PAGE_ID_NONE);
    const tree_meta m = {rid, 1, 0};
    meta_encode(mp, &m);
    *meta_id = mid;
    return 0;
}

int bt_open(ps_store *s, ps_page_id meta_id, bt_tree *t)
{
    const u32 page_size = ps_page_size(s);
    const u32 cost = max_cost(page_size);
    /* The image, cell and separator buffers. See image_buf. */
    u8 *scratch = malloc((usize) page_size + 2u * cost);
    if (scratch == NULL) {
        return ENOMEM;
    }
    t->store = s;
    t->meta_id = meta_id;
    t->page_size = page_size;
    t->max_key = cost - SLOT_SIZE - INNER_CELL_HEADER;
    t->max_entry = cost - SLOT_SIZE - LEAF_CELL_HEADER;
    t->scratch = scratch;
    return 0;
}

void bt_close(bt_tree *t)
{
    free(t->scratch);
    t->scratch = NULL;
}

/* ---- snapshot reads ---------------------------------------------------------------- */

static int snap_node(const bt_tree *t, ps_snapshot *snap, ps_page_id id, u32 level, const u8 **out)
{
    const ps_page *p;
    const int rv = ps_snapshot_read(snap, id, &p);
    if (rv != 0) {
        return rv;
    }
    if (!node_sane(t->page_size, (const u8 *) p, level)) {
        ps_snapshot_unpin(snap, id);
        return EBADMSG;
    }
    *out = (const u8 *) p;
    return 0;
}

static int snap_meta(const bt_tree *t, ps_snapshot *snap, tree_meta *m)
{
    const ps_page *p;
    int rv = ps_snapshot_read(snap, t->meta_id, &p);
    if (rv != 0) {
        return rv;
    }
    rv = meta_decode(p, m);
    ps_snapshot_unpin(snap, t->meta_id);
    return rv;
}

/* Finds the leaf whose range holds key and returns it pinned. */
static int snap_leaf(
    const bt_tree *t,
    ps_snapshot *snap,
    const u8 *key,
    u32 klen,
    ps_page_id *leaf_id,
    const u8 **leaf)
{
    tree_meta m;
    int rv = snap_meta(t, snap, &m);
    if (rv != 0) {
        return rv;
    }
    ps_page_id id = m.root;
    for (u32 level = m.height - 1;; level--) {
        const u8 *n;
        rv = snap_node(t, snap, id, level, &n);
        if (rv != 0) {
            return rv;
        }
        if (level == 0) {
            *leaf_id = id;
            *leaf = n;
            return 0;
        }
        const ps_page_id child = n_child(n, inner_find(n, key, klen));
        ps_snapshot_unpin(snap, id);
        id = child;
    }
}

int bt_get(
    const bt_tree *t,
    ps_snapshot *snap,
    const void *key,
    u32 klen,
    void *buf,
    u32 cap,
    u32 *vlen)
{
    if ((key == NULL && klen > 0) || (buf == NULL && cap > 0)) {
        return EINVAL;
    }
    ps_page_id id;
    const u8 *leaf;
    const int rv = snap_leaf(t, snap, key, klen, &id, &leaf);
    if (rv != 0) {
        return rv;
    }
    bool found;
    const u32 i = node_lower(leaf, key, klen, &found);
    if (found) {
        const u8 *c = n_cell(leaf, i);
        const u32 len = get16(c + 2);
        copy(buf, c + LEAF_CELL_HEADER + get16(c), len < cap ? len : cap);
        *vlen = len;
    }
    ps_snapshot_unpin(snap, id);
    return found ? 0 : ENOENT;
}

int bt_cursor_open(bt_cursor *c, const bt_tree *t, ps_snapshot *snap, const void *start, u32 slen)
{
    c->tree = t;
    c->snap = snap;
    c->leaf = PS_PAGE_ID_NONE;
    c->slot = 0;
    c->page = NULL;
    if (start == NULL && slen > 0) {
        return EINVAL;
    }
    ps_page_id id;
    const u8 *leaf;
    const int rv = snap_leaf(t, snap, start, slen, &id, &leaf);
    if (rv != 0) {
        return rv;
    }
    bool found;
    c->leaf = id;
    c->page = leaf;
    c->slot = node_lower(leaf, start, slen, &found);
    return 0;
}

int bt_cursor_next(bt_cursor *c, bt_item *out)
{
    /* Leaves can be empty, so keep following links until an entry turns up. */
    while (!ps_page_id_is_none(c->leaf) && c->slot >= n_count(c->page)) {
        const ps_page_id next = n_link(c->page);
        bt_cursor_close(c);
        if (ps_page_id_is_none(next)) {
            break;
        }
        const u8 *n;
        const int rv = snap_node(c->tree, c->snap, next, 0, &n);
        if (rv != 0) {
            return rv;
        }
        c->leaf = next;
        c->page = n;
        c->slot = 0;
    }
    if (ps_page_id_is_none(c->leaf)) {
        return ENOENT;
    }
    const u8 *cell = n_cell(c->page, c->slot++);
    out->klen = get16(cell);
    out->vlen = get16(cell + 2);
    out->key = cell + LEAF_CELL_HEADER;
    out->val = cell + LEAF_CELL_HEADER + out->klen;
    return 0;
}

void bt_cursor_close(bt_cursor *c)
{
    if (!ps_page_id_is_none(c->leaf)) {
        ps_snapshot_unpin(c->snap, c->leaf);
    }
    c->leaf = PS_PAGE_ID_NONE;
    c->page = NULL;
    c->slot = 0;
}

/* ---- structure check ---------------------------------------------------------------- */

/* Keys in a subtree fall in [lo, hi). A NULL bound is open. */
typedef struct key_range {
    const u8 *lo;
    u32 lolen;
    const u8 *hi;
    u32 hilen;
} key_range;

typedef struct check_state {
    const bt_tree *t;
    ps_snapshot *snap;
    bt_stats *st;
    char *msg;
    usize msg_cap;
    u32 height;
    ps_page_id path[BT_MAX_HEIGHT];
    bool seen_leaf;
    ps_page_id next_leaf; /* link of the last leaf visited */
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

static bool below(const u8 *key, u32 klen, const u8 *bound, u32 blen)
{
    return key_cmp(key, klen, bound, blen) < 0;
}

/* Checks cell bounds, key order and byte accounting within one node. */
static int check_cells(check_state *c, ps_page_id id, const u8 *n, const key_range *range)
{
    const u32 page_size = c->t->page_size;
    const bool leaf = n_leaf(n);
    const u32 header = leaf ? LEAF_CELL_HEADER : INNER_CELL_HEADER;
    u32 bytes = 0;
    const u8 *prev = NULL;
    u32 prevlen = 0;
    for (u32 i = 0; i < n_count(n); i++) {
        const u32 off = n_slot(n, i);
        if (off < n_cell_start(n) || off + header > page_size ||
            off + cell_size(leaf, n + off) > page_size) {
            return check_fail(c, id, "cell %u at offset %u runs off the page", i, off);
        }
        bytes += cell_size(leaf, n + off);
        u32 klen;
        const u8 *key = cell_key(leaf, n + off, &klen);
        if (klen > c->t->max_key) {
            return check_fail(c, id, "key %u is %u bytes long", i, klen);
        }
        if (prev != NULL && !below(prev, prevlen, key, klen)) {
            return check_fail(c, id, "key %u is out of order", i);
        }
        if (range->lo != NULL && below(key, klen, range->lo, range->lolen)) {
            return check_fail(c, id, "key %u sorts below the parent's separator", i);
        }
        if (range->hi != NULL && !below(key, klen, range->hi, range->hilen)) {
            return check_fail(c, id, "key %u sorts at or above the next separator", i);
        }
        if (!leaf && ps_page_id_is_none(cell_child(n + off))) {
            return check_fail(c, id, "cell %u has no child", i);
        }
        prev = key;
        prevlen = klen;
    }
    if (!leaf && ps_page_id_is_none(n_link(n))) {
        return check_fail(c, id, "the leftmost child is missing");
    }
    if (bytes + n_garbage(n) != page_size - n_cell_start(n)) {
        return check_fail(
            c,
            id,
            "cells take %u bytes and garbage %u, but the cell area has %u",
            bytes,
            n_garbage(n),
            page_size - n_cell_start(n));
    }
    return 0;
}

static int check_node(check_state *c, ps_page_id id, u32 depth, const key_range *range)
{
    for (u32 d = 0; d < depth; d++) {
        if (ps_page_id_eq(c->path[d], id)) {
            return check_fail(c, id, "appears twice on one path from the root");
        }
    }
    const u32 level = c->height - 1 - depth;
    const u8 *n;
    int rv = snap_node(c->t, c->snap, id, level, &n);
    if (rv == EBADMSG) {
        return check_fail(c, id, "bad node header for level %u", level);
    }
    if (rv != 0) {
        return rv;
    }
    c->path[depth] = id;
    rv = check_cells(c, id, n, range);
    if (rv == 0 && level == 0) {
        if (c->seen_leaf && !ps_page_id_eq(c->next_leaf, id)) {
            rv = check_fail(c, id, "the previous leaf links to page %u instead", c->next_leaf.raw);
        }
        c->seen_leaf = true;
        c->next_leaf = n_link(n);
        c->st->keys += n_count(n);
        c->st->leaf_pages++;
        c->st->leaf_bytes += n_used(c->t, n);
    } else if (rv == 0) {
        c->st->inner_pages++;
        c->st->inner_bytes += n_used(c->t, n);
        for (u32 ci = 0; rv == 0 && ci <= n_count(n); ci++) {
            key_range sub = *range;
            if (ci > 0) {
                sub.lo = n_key(n, ci - 1, &sub.lolen);
            }
            if (ci < n_count(n)) {
                sub.hi = n_key(n, ci, &sub.hilen);
            }
            rv = check_node(c, n_child(n, ci), depth + 1, &sub);
        }
    }
    ps_snapshot_unpin(c->snap, id);
    return rv;
}

int bt_check(const bt_tree *t, ps_snapshot *snap, bt_stats *out, char *msg, usize msg_cap)
{
    memset(out, 0, sizeof(*out));
    if (msg_cap > 0) {
        msg[0] = '\0';
    }
    out->node_capacity = capacity(t);
    check_state c = {.t = t, .snap = snap, .st = out, .msg = msg, .msg_cap = msg_cap};
    tree_meta m;
    int rv = snap_meta(t, snap, &m);
    if (rv == EBADMSG) {
        return check_fail(&c, t->meta_id, "bad meta page");
    }
    if (rv != 0) {
        return rv;
    }
    c.height = m.height;
    out->height = m.height;
    const key_range all = {NULL, 0, NULL, 0};
    rv = check_node(&c, m.root, 0, &all);
    if (rv != 0) {
        return rv;
    }
    if (!ps_page_id_is_none(c.next_leaf)) {
        return check_fail(&c, c.next_leaf, "the last leaf links here");
    }
    if (out->keys != m.count) {
        return check_fail(
            &c,
            t->meta_id,
            "the tree holds %llu keys but the meta page says %llu",
            (unsigned long long) out->keys,
            (unsigned long long) m.count);
    }
    return 0;
}
