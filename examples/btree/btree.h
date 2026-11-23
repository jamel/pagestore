#pragma once

/*
 * A B+tree that maps byte-string keys to byte-string values, stored in
 * PageStore pages. Keys sort like memcmp, and a key sorts before every
 * longer key that starts with it.
 *
 * Writes run inside the caller's store operation, so the caller picks the
 * LSN, logs the change and then commits or aborts. A failed write can leave
 * the tree half changed, so abort the operation after any error. Reads go
 * through a snapshot and see the tree as it was when the snapshot opened.
 *
 * One thread writes at a time. Readers on other threads can share the
 * bt_tree, each with its own snapshot. Reads unpin pages by ID, so while a
 * cursor is open, read its snapshot only through that cursor.
 *
 * Size limits depend on the page size. With 4 KiB pages a key can hold up
 * to 1006 bytes, and a key and its value together up to 1008.
 *
 * Usage examples
 *
 * Create the tree in a new store's first operation. Keep the meta page ID,
 * because later opens need it.
 *
 *     u32 meta;
 *     int rv = ps_op_begin(s, 1);
 *     if (rv == 0) {
 *         rv = bt_create(s, &meta);
 *         if (rv == 0) {
 *             ps_op_commit(s);
 *         } else {
 *             ps_op_abort(s);
 *         }
 *     }
 *
 * Insert a value, or replace the one already stored under the key.
 *
 *     int put(bt_tree *t, u64 lsn, const char *k, const char *v)
 *     {
 *         int rv = ps_op_begin(t->store, lsn);
 *         if (rv != 0) {
 *             return rv;
 *         }
 *         rv = bt_put(t, k, (u32) strlen(k), v, (u32) strlen(v));
 *         if (rv == 0) {
 *             ps_op_commit(t->store);
 *         } else {
 *             ps_op_abort(t->store);
 *         }
 *         return rv;
 *     }
 *
 * Print every key from "b" on. ENOENT from bt_cursor_next marks the end.
 *
 *     bt_cursor c;
 *     int rv = bt_cursor_open(&c, t, snap, "b", 1);
 *     if (rv == 0) {
 *         for (;;) {
 *             bt_item it;
 *             rv = bt_cursor_next(&c, &it);
 *             if (rv != 0) {
 *                 break;
 *             }
 *             printf("%.*s\n", (int) it.klen, (const char *) it.key);
 *         }
 *     }
 *     bt_cursor_close(&c);
 */

#include "pagestore/pagestore.h"

PS_EXTERN_C_BEGIN

/* Deeper trees return EOVERFLOW. Real data stays far below this. */
#define BT_MAX_HEIGHT 32u

/* Set up by bt_open. Callers may read the fields but not change them. */
typedef struct bt_tree {
    ps_store *store;
    u32 meta_id;
    u32 page_size;
    u32 max_key; /* longest key in bytes */
    u32 max_entry; /* longest key plus value in bytes */
    u8 *scratch; /* writer workspace, private to btree.c */
} bt_tree;

/* An entry seen through a cursor. The pointers last until the next cursor call. */
typedef struct bt_item {
    const u8 *key;
    u32 klen;
    const u8 *val;
    u32 vlen;
} bt_item;

/* Caller-owned. The fields are private to btree.c. */
typedef struct bt_cursor {
    const bt_tree *tree;
    ps_snapshot *snap;
    u32 leaf; /* pinned leaf, 0 once the cursor is past the end */
    u32 slot;
    const u8 *page;
} bt_cursor;

/* Filled in by bt_check. */
typedef struct bt_stats {
    u64 keys;
    u32 height; /* 1 when the root is a leaf */
    u64 leaf_pages;
    u64 inner_pages;
    u64 leaf_bytes; /* bytes that leaf entries use */
    u64 inner_bytes; /* bytes that inner entries use */
    u32 node_capacity; /* bytes for entries in one node */
} bt_stats;

/*
 * Allocates an empty tree (a meta page and a root leaf) in the current
 * operation and returns the meta page ID.
 */
int bt_create(ps_store *s, u32 *meta_id);

/*
 * Sets up a handle for the tree whose meta page is meta_id. It reads no
 * pages, so a tree created by a later operation, such as one replayed from
 * the log, can use it. ENOMEM if the writer workspace can't be allocated.
 */
int bt_open(ps_store *s, u32 meta_id, bt_tree *t);
void bt_close(bt_tree *t);

/*
 * Inserts key or replaces its value, in the current operation. EINVAL if
 * the key or the entry is too long.
 */
int bt_put(bt_tree *t, const void *key, u32 klen, const void *val, u32 vlen);

/* Removes key in the current operation. ENOENT if it is missing. */
int bt_delete(bt_tree *t, const void *key, u32 klen);

/*
 * Copies up to cap bytes of the value into buf and sets *vlen to its full
 * length. ENOENT if the key is missing.
 */
int bt_get(
    const bt_tree *t,
    ps_snapshot *snap,
    const void *key,
    u32 klen,
    void *buf,
    u32 cap,
    u32 *vlen);

/*
 * Positions the cursor before the first key at or after start. Pass an
 * empty start to begin at the smallest key. Close the cursor even if this
 * fails.
 */
int bt_cursor_open(bt_cursor *c, const bt_tree *t, ps_snapshot *snap, const void *start, u32 slen);

/* Returns the next entry, or ENOENT at the end of the tree. */
int bt_cursor_next(bt_cursor *c, bt_item *out);

/* Releases the cursor's pin. Safe to call more than once. */
void bt_cursor_close(bt_cursor *c);

/*
 * Checks the whole tree's structure and fills in *out. A broken tree returns
 * EBADMSG and describes the first problem in msg. It holds one pin per
 * level, so the snapshot needs a pin cap above the height.
 */
int bt_check(const bt_tree *t, ps_snapshot *snap, bt_stats *out, char *msg, usize msg_cap);

PS_EXTERN_C_END
