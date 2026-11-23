/*
 * Message log pages. Integers use host byte order, which PageStore
 * requires to be little-endian.
 *
 * Meta page ("MLMT"), one per log. Its ID is the log's entry point.
 *
 * Offset   +0..3                  +4..7
 *       +---------------------------------------------+
 *  0x00 | ps_page header (24 bytes)                   |
 *       +---------------------------------------------+
 *  0x18 | cap                                         |
 *  0x20 | first_seq                                   |
 *  0x28 | next_seq                                    |
 *  0x30 | first_off                                   |
 *  0x38 | end_off                                     |
 *       +----------------------+----------------------+
 *  0x40 | tail page ID         | reader count         |
 *  0x48 | index root           | index height         |
 *  0x50 | page map root        | page map height      |
 *       +----------------------+----------------------+
 *  0x58 | reader slots, 32 bytes each, to the end     |
 *       +---------------------------------------------+
 *
 * Reader slot. A zero length marks a free slot.
 *
 *       +------+--------------------------------------+
 *  +0   | len  | name, padded with zeros (23 bytes)   |
 *       +------+--------------------------------------+
 *  +24  | position                                    |
 *       +---------------------------------------------+
 *
 * Data page ("MLDA").
 *
 *       +---------------------------------------------+
 *  0x00 | ps_page header (24 bytes)                   |
 *       +---------------------------------------------+
 *  0x18 | logical page number                         |
 *       +---------------------------------------------+
 *  0x20 | payload bytes, to the end of the page       |
 *       +---------------------------------------------+
 *
 * Byte o of the logical space is in logical page o / D at payload offset
 * o % D, where D is the payload size of a data page. first_off is where
 * the oldest kept message starts, and end_off where the next one will.
 * The live data pages are first_off / D up to, but not including,
 * ceil(end_off / D). The tail page is the last live one, or 0 when none
 * is live, and saves a page map lookup on every push.
 *
 * Implementation notes:
 *
 * - A message ends where the next one starts, or at end_off for the newest.
 * - A trim frees the data pages below the new oldest message. Old bytes
 *   before first_off can remain in the first live page.
 * - Abort rolls back every page the writer changed, so no change needs
 *   undoing here.
 */

#include "msglog.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ---- page layout ---------------------------------------------------------- */

#define META_MAGIC 0x544D4C4Du /* "MLMT" little-endian */
#define DATA_MAGIC 0x41444C4Du /* "MLDA" */

#define META_CAP 0x18
#define META_FIRST_SEQ 0x20
#define META_NEXT_SEQ 0x28
#define META_FIRST_OFF 0x30
#define META_END_OFF 0x38
#define META_TAIL 0x40
#define META_READERS 0x44
#define META_INDEX_ROOT 0x48
#define META_INDEX_HEIGHT 0x4C
#define META_MAP_ROOT 0x50
#define META_MAP_HEIGHT 0x54
#define META_SLOTS 0x58

#define SLOT_SIZE 32u
#define SLOT_NAME 1
#define SLOT_POS 24

#define DATA_LOGICAL 0x18
#define DATA_PAYLOAD 0x20

PS_STATIC_ASSERT(META_CAP == PS_PAGE_HEADER_SIZE, "log fields follow the page header");
PS_STATIC_ASSERT(DATA_LOGICAL == PS_PAGE_HEADER_SIZE, "data fields follow the page header");
PS_STATIC_ASSERT(SLOT_NAME + ML_NAME_MAX == SLOT_POS, "the name fills the slot up to the position");

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

static u32 min32(u64 a, u64 b)
{
    return (u32) (a < b ? a : b);
}

/* ---- meta page ------------------------------------------------------------ */

typedef struct meta {
    u64 cap;
    u64 first_seq;
    u64 next_seq;
    u64 first_off;
    u64 end_off;
    u32 tail;
    u32 readers;
    u32 index_root;
    u32 index_height;
    u32 map_root;
    u32 map_height;
} meta;

static int meta_decode(const ps_page *p, meta *m)
{
    const u8 *b = (const u8 *) p;
    if (p->magic != META_MAGIC) {
        return EBADMSG;
    }
    m->cap = get64(b + META_CAP);
    m->first_seq = get64(b + META_FIRST_SEQ);
    m->next_seq = get64(b + META_NEXT_SEQ);
    m->first_off = get64(b + META_FIRST_OFF);
    m->end_off = get64(b + META_END_OFF);
    m->tail = get32(b + META_TAIL);
    m->readers = get32(b + META_READERS);
    m->index_root = get32(b + META_INDEX_ROOT);
    m->index_height = get32(b + META_INDEX_HEIGHT);
    m->map_root = get32(b + META_MAP_ROOT);
    m->map_height = get32(b + META_MAP_HEIGHT);
    return 0;
}

static void meta_encode(ps_page *p, const meta *m)
{
    u8 *b = (u8 *) p;
    p->magic = META_MAGIC;
    put64(b + META_CAP, m->cap);
    put64(b + META_FIRST_SEQ, m->first_seq);
    put64(b + META_NEXT_SEQ, m->next_seq);
    put64(b + META_FIRST_OFF, m->first_off);
    put64(b + META_END_OFF, m->end_off);
    put32(b + META_TAIL, m->tail);
    put32(b + META_READERS, m->readers);
    put32(b + META_INDEX_ROOT, m->index_root);
    put32(b + META_INDEX_HEIGHT, m->index_height);
    put32(b + META_MAP_ROOT, m->map_root);
    put32(b + META_MAP_HEIGHT, m->map_height);
}

/* The radix arrays with the roots recorded in m. */
static rx_array index_of(const ml_log *l, const meta *m)
{
    rx_array a = l->index;
    a.root = m->index_root;
    a.height = m->index_height;
    return a;
}

static rx_array map_of(const ml_log *l, const meta *m)
{
    rx_array a = l->pages;
    a.root = m->map_root;
    a.height = m->map_height;
    return a;
}

static void save_roots(meta *m, const rx_array *index, const rx_array *pages)
{
    m->index_root = index->root;
    m->index_height = index->height;
    m->map_root = pages->root;
    m->map_height = pages->height;
}

/* Logical pages that hold bytes below end_off. */
static u64 page_count(const ml_log *l, u64 end_off)
{
    return end_off / l->data_size + (end_off % l->data_size != 0);
}

static int snap_meta(const ml_log *l, ps_snapshot *snap, meta *m)
{
    const ps_page *p;
    int rv = ps_snapshot_read(snap, l->meta_id, &p);
    if (rv != 0) {
        return rv;
    }
    rv = meta_decode(p, m);
    ps_snapshot_unpin(snap, l->meta_id);
    return rv;
}

/* Gets the meta page for writing in the current operation. */
static int write_meta(ml_log *l, ps_page **p, meta *m)
{
    const int rv = ps_write(l->store, l->meta_id, p);
    return rv != 0 ? rv : meta_decode(*p, m);
}

static bool data_ok(const ps_page *p, u64 logical)
{
    return p->magic == DATA_MAGIC && get64((const u8 *) p + DATA_LOGICAL) == logical;
}

/* ---- create and open ------------------------------------------------------ */

int ml_create(ps_store *s, u64 cap, u32 *meta_id)
{
    if (cap == 0) {
        return EINVAL;
    }
    ps_page *p;
    const int rv = ps_alloc(s, meta_id, &p);
    if (rv != 0) {
        return rv;
    }
    const meta m = {.cap = cap};
    meta_encode(p, &m);
    return 0;
}

void ml_open(ps_store *s, u32 meta_id, ml_log *l)
{
    l->store = s;
    l->meta_id = meta_id;
    l->page_size = ps_page_size(s);
    l->data_size = l->page_size - DATA_PAYLOAD;
    l->max_readers = (l->page_size - META_SLOTS) / SLOT_SIZE;
    int rv = rx_init(&l->index, l->page_size, 8);
    if (rv == 0) {
        rv = rx_init(&l->pages, l->page_size, 4);
    }
    assert(rv == 0); /* every store page size fits */
    PS_UNUSED(rv);
}

/* ---- push ----------------------------------------------------------------- */

/* Copies msg to the end of the byte space, adding data pages as needed. */
static int append_bytes(ml_log *l, meta *m, rx_array *pages, const u8 *msg, u32 len)
{
    const u32 d = l->data_size;
    u32 done = 0;
    while (done < len) {
        const u64 lp = m->end_off / d;
        const u32 at = (u32) (m->end_off % d);
        ps_page *p;
        int rv;
        if (at == 0) {
            u32 id;
            rv = ps_alloc(l->store, &id, &p);
            if (rv == 0) {
                rv = rx_set(pages, l->store, lp, id);
            }
            if (rv != 0) {
                return rv;
            }
            p->magic = DATA_MAGIC;
            put64((u8 *) p + DATA_LOGICAL, lp);
            m->tail = id;
        } else {
            rv = ps_write(l->store, m->tail, &p);
            if (rv != 0) {
                return rv;
            }
            if (!data_ok(p, lp)) {
                return EBADMSG;
            }
        }
        const u32 n = min32(len - done, d - at);
        memcpy((u8 *) p + DATA_PAYLOAD + at, msg + done, n);
        done += n;
        m->end_off += n;
    }
    return 0;
}

/* Drops the messages below new_first and frees the pages only they used. */
static int trim(ml_log *l, meta *m, rx_array *index, rx_array *pages, u64 new_first)
{
    u64 off;
    int rv = rx_get_op(index, l->store, new_first, &off);
    if (rv != 0) {
        return rv == ENOENT ? EBADMSG : rv;
    }
    const u64 from_page = m->first_off / l->data_size;
    const u64 to_page = off / l->data_size;
    for (u64 lp = from_page; lp < to_page; lp++) {
        u64 id;
        rv = rx_get_op(pages, l->store, lp, &id);
        if (rv == 0 && id == 0) {
            rv = EBADMSG;
        }
        if (rv == 0) {
            rv = ps_free(l->store, (u32) id);
        }
        if (rv != 0) {
            return rv == ENOENT ? EBADMSG : rv;
        }
    }
    rv = rx_trim(pages, l->store, from_page, to_page);
    if (rv == 0) {
        rv = rx_trim(index, l->store, m->first_seq, new_first);
    }
    if (rv != 0) {
        return rv;
    }
    if (to_page >= page_count(l, m->end_off)) {
        m->tail = 0;
    }
    m->first_seq = new_first;
    m->first_off = off;
    return 0;
}

int ml_push(ml_log *l, const void *msg, u32 len, u64 *seq)
{
    if (msg == NULL && len > 0) {
        return EINVAL;
    }
    ps_page *mp;
    meta m;
    int rv = write_meta(l, &mp, &m);
    if (rv != 0) {
        return rv;
    }
    if (m.end_off > UINT64_MAX - len || m.next_seq == UINT64_MAX) {
        return EOVERFLOW;
    }
    rx_array index = index_of(l, &m);
    rx_array pages = map_of(l, &m);
    rv = rx_set(&index, l->store, m.next_seq, m.end_off);
    if (rv == 0) {
        rv = append_bytes(l, &m, &pages, msg, len);
    }
    if (rv != 0) {
        return rv;
    }
    *seq = m.next_seq++;
    if (m.next_seq - m.first_seq > m.cap) {
        rv = trim(l, &m, &index, &pages, m.next_seq - m.cap);
        if (rv != 0) {
            return rv;
        }
    }
    save_roots(&m, &index, &pages);
    meta_encode(mp, &m);
    return 0;
}

/* ---- reads ---------------------------------------------------------------- */

int ml_state_get(const ml_log *l, ps_snapshot *snap, ml_state *out)
{
    meta m;
    const int rv = snap_meta(l, snap, &m);
    if (rv != 0) {
        return rv;
    }
    out->cap = m.cap;
    out->first_seq = m.first_seq;
    out->next_seq = m.next_seq;
    out->bytes = m.end_off - m.first_off;
    out->readers = m.readers;
    return 0;
}

/* Copies n bytes from offset off of the byte space. */
static int copy_bytes(
    const ml_log *l,
    ps_snapshot *snap,
    const rx_array *pages,
    u64 off,
    u32 n,
    u8 *out)
{
    const u32 d = l->data_size;
    u32 done = 0;
    while (done < n) {
        const u64 lp = off / d;
        const u32 at = (u32) (off % d);
        u64 id;
        int rv = rx_get(pages, snap, lp, &id);
        if (rv == 0 && id == 0) {
            rv = EBADMSG;
        }
        if (rv != 0) {
            return rv == ENOENT ? EBADMSG : rv;
        }
        const ps_page *p;
        rv = ps_snapshot_read(snap, (u32) id, &p);
        if (rv != 0) {
            return rv;
        }
        if (!data_ok(p, lp)) {
            ps_snapshot_unpin(snap, (u32) id);
            return EBADMSG;
        }
        const u32 k = min32(n - done, d - at);
        memcpy(out + done, (const u8 *) p + DATA_PAYLOAD + at, k);
        ps_snapshot_unpin(snap, (u32) id);
        done += k;
        off += k;
    }
    return 0;
}

int ml_read(const ml_log *l, ps_snapshot *snap, u64 seq, void *buf, u32 cap, u32 *len)
{
    if (buf == NULL && cap > 0) {
        return EINVAL;
    }
    meta m;
    int rv = snap_meta(l, snap, &m);
    if (rv != 0) {
        return rv;
    }
    if (seq < m.first_seq || seq >= m.next_seq) {
        return ENOENT;
    }
    const rx_array index = index_of(l, &m);
    const rx_array pages = map_of(l, &m);
    u64 off;
    u64 end = m.end_off;
    rv = rx_get(&index, snap, seq, &off);
    if (rv == 0 && seq + 1 < m.next_seq) {
        rv = rx_get(&index, snap, seq + 1, &end);
    }
    if (rv != 0) {
        return rv == ENOENT ? EBADMSG : rv;
    }
    if (off < m.first_off || off > end || end > m.end_off || end - off > UINT32_MAX) {
        return EBADMSG;
    }
    *len = (u32) (end - off);
    return copy_bytes(l, snap, &pages, off, min32(*len, cap), buf);
}

/* ---- readers -------------------------------------------------------------- */

static bool name_ok(const char *name, usize *len)
{
    if (name == NULL) {
        return false;
    }
    *len = strlen(name);
    return *len >= 1 && *len <= ML_NAME_MAX;
}

static const u8 *slot_at(const ps_page *p, u32 i)
{
    return (const u8 *) p + META_SLOTS + (usize) i * SLOT_SIZE;
}

static u8 *slot_mut(ps_page *p, u32 i)
{
    return (u8 *) p + META_SLOTS + (usize) i * SLOT_SIZE;
}

/* ENOENT if no slot holds name. */
static int find_slot(const ml_log *l, const ps_page *p, const char *name, usize len, u32 *out)
{
    for (u32 i = 0; i < l->max_readers; i++) {
        const u8 *slot = slot_at(p, i);
        if (slot[0] == len && memcmp(slot + SLOT_NAME, name, len) == 0) {
            *out = i;
            return 0;
        }
    }
    return ENOENT;
}

int ml_reader_add(ml_log *l, const char *name, u64 pos)
{
    usize len;
    if (!name_ok(name, &len)) {
        return EINVAL;
    }
    ps_page *mp;
    meta m;
    int rv = write_meta(l, &mp, &m);
    if (rv != 0) {
        return rv;
    }
    u32 i;
    if (find_slot(l, mp, name, len, &i) == 0) {
        return EEXIST;
    }
    for (i = 0; i < l->max_readers && slot_at(mp, i)[0] != 0; i++) {}
    if (i == l->max_readers) {
        return ENOSPC;
    }
    pos = pos < m.first_seq ? m.first_seq : pos > m.next_seq ? m.next_seq : pos;
    u8 *slot = slot_mut(mp, i);
    memset(slot, 0, SLOT_SIZE);
    slot[0] = (u8) len;
    memcpy(slot + SLOT_NAME, name, len);
    put64(slot + SLOT_POS, pos);
    m.readers++;
    meta_encode(mp, &m);
    return 0;
}

int ml_reader_remove(ml_log *l, const char *name)
{
    usize len;
    if (!name_ok(name, &len)) {
        return ENOENT;
    }
    ps_page *mp;
    meta m;
    int rv = write_meta(l, &mp, &m);
    u32 i;
    if (rv == 0) {
        rv = find_slot(l, mp, name, len, &i);
    }
    if (rv != 0) {
        return rv;
    }
    memset(slot_mut(mp, i), 0, SLOT_SIZE);
    m.readers--;
    meta_encode(mp, &m);
    return 0;
}

int ml_reader_ack(ml_log *l, const char *name, u64 pos)
{
    usize len;
    if (!name_ok(name, &len)) {
        return ENOENT;
    }
    ps_page *mp;
    meta m;
    int rv = write_meta(l, &mp, &m);
    u32 i;
    if (rv == 0) {
        rv = find_slot(l, mp, name, len, &i);
    }
    if (rv != 0) {
        return rv;
    }
    if (pos > m.next_seq) {
        return EINVAL;
    }
    u8 *slot = slot_mut(mp, i);
    if (pos > get64(slot + SLOT_POS)) {
        put64(slot + SLOT_POS, pos);
    }
    return 0;
}

int ml_reader_get(const ml_log *l, ps_snapshot *snap, const char *name, u64 *pos)
{
    usize len;
    if (!name_ok(name, &len)) {
        return ENOENT;
    }
    const ps_page *p;
    int rv = ps_snapshot_read(snap, l->meta_id, &p);
    if (rv != 0) {
        return rv;
    }
    u32 i;
    rv = p->magic != META_MAGIC ? EBADMSG : find_slot(l, p, name, len, &i);
    if (rv == 0) {
        *pos = get64(slot_at(p, i) + SLOT_POS);
    }
    ps_snapshot_unpin(snap, l->meta_id);
    return rv;
}

int ml_reader_next(
    const ml_log *l,
    ps_snapshot *snap,
    u32 *slot,
    char name[ML_NAME_MAX + 1],
    u64 *pos)
{
    const ps_page *p;
    int rv = ps_snapshot_read(snap, l->meta_id, &p);
    if (rv != 0) {
        return rv;
    }
    rv = p->magic != META_MAGIC ? EBADMSG : ENOENT;
    for (u32 i = *slot; rv == ENOENT && i < l->max_readers; i++) {
        const u8 *s = slot_at(p, i);
        if (s[0] != 0) {
            const u32 len = s[0] <= ML_NAME_MAX ? s[0] : ML_NAME_MAX;
            memcpy(name, s + SLOT_NAME, len);
            name[len] = '\0';
            *pos = get64(s + SLOT_POS);
            *slot = i + 1;
            rv = 0;
        }
    }
    ps_snapshot_unpin(snap, l->meta_id);
    return rv;
}

/* ---- structure check ------------------------------------------------------ */

static int fail(char *msg, usize msg_cap, const char *fmt, ...) PS_PRINTF(3, 4);

static int fail(char *msg, usize msg_cap, const char *fmt, ...)
{
    if (msg_cap > 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, msg_cap, fmt, ap);
        va_end(ap);
    }
    return EBADMSG;
}

/* Checks the counters and the reader table on the meta page. */
static int check_meta(const ml_log *l, const ps_page *p, const meta *m, char *msg, usize cap)
{
    if (m->cap == 0 || m->first_seq > m->next_seq || m->next_seq - m->first_seq > m->cap) {
        return fail(
            msg,
            cap,
            "meta: %llu..%llu doesn't fit cap %llu",
            (unsigned long long) m->first_seq,
            (unsigned long long) m->next_seq,
            (unsigned long long) m->cap);
    }
    if (m->first_off > m->end_off || (m->first_seq == m->next_seq && m->first_off != m->end_off)) {
        return fail(
            msg,
            cap,
            "meta: bad byte range %llu..%llu",
            (unsigned long long) m->first_off,
            (unsigned long long) m->end_off);
    }
    u32 used = 0;
    for (u32 i = 0; i < l->max_readers; i++) {
        const u8 *s = slot_at(p, i);
        if (s[0] == 0) {
            continue;
        }
        used++;
        if (s[0] > ML_NAME_MAX || memchr(s + SLOT_NAME, 0, s[0]) != NULL) {
            return fail(msg, cap, "reader slot %u: bad name", i);
        }
        if (get64(s + SLOT_POS) > m->next_seq) {
            return fail(msg, cap, "reader slot %u: position past the newest message", i);
        }
        for (u32 j = 0; j < i; j++) {
            if (memcmp(slot_at(p, j), s, SLOT_POS) == 0) {
                return fail(msg, cap, "reader slot %u: same name as slot %u", i, j);
            }
        }
    }
    if (used != m->readers) {
        return fail(msg, cap, "meta: %u readers in use, count says %u", used, m->readers);
    }
    return 0;
}

static int check_array(
    const char *what,
    const rx_array *a,
    ps_snapshot *snap,
    u64 lo,
    u64 hi,
    u64 *pages,
    char *msg,
    usize cap)
{
    rx_stats st;
    char why[200];
    const int rv = rx_check(a, snap, lo, hi, &st, why, sizeof(why));
    if (rv == EBADMSG) {
        return fail(msg, cap, "%s: %s", what, why);
    }
    *pages = st.leaves + st.inner;
    return rv;
}

/* Offsets must start at first_off and never decrease or pass end_off. */
static int check_offsets(
    const rx_array *index,
    ps_snapshot *snap,
    const meta *m,
    char *msg,
    usize cap)
{
    u64 prev = m->first_off;
    for (u64 seq = m->first_seq; seq < m->next_seq; seq++) {
        u64 off;
        const int rv = rx_get(index, snap, seq, &off);
        if (rv != 0) {
            return rv == ENOENT ?
                fail(msg, cap, "index: no entry for %llu", (unsigned long long) seq) :
                rv;
        }
        const bool bad = seq == m->first_seq ? off != m->first_off : off < prev;
        if (bad || off > m->end_off) {
            return fail(
                msg,
                cap,
                "index: message %llu at offset %llu, after %llu",
                (unsigned long long) seq,
                (unsigned long long) off,
                (unsigned long long) prev);
        }
        prev = off;
    }
    return 0;
}

static int check_data(
    const ml_log *l,
    const rx_array *pages,
    ps_snapshot *snap,
    const meta *m,
    u64 *count,
    char *msg,
    usize cap)
{
    const u64 first = m->first_off / l->data_size;
    const u64 n = page_count(l, m->end_off);
    *count = 0;
    for (u64 lp = first; lp < n; lp++) {
        u64 id;
        int rv = rx_get(pages, snap, lp, &id);
        if (rv == ENOENT || (rv == 0 && id == 0)) {
            return fail(
                msg,
                cap,
                "page map: no page for logical page %llu",
                (unsigned long long) lp);
        }
        if (rv != 0) {
            return rv;
        }
        if (lp == n - 1 && id != m->tail) {
            return fail(
                msg,
                cap,
                "meta: tail page %u, page map says %llu",
                m->tail,
                (unsigned long long) id);
        }
        const ps_page *p;
        rv = ps_snapshot_read(snap, (u32) id, &p);
        if (rv != 0) {
            return rv;
        }
        const bool ok = data_ok(p, lp);
        ps_snapshot_unpin(snap, (u32) id);
        if (!ok) {
            return fail(
                msg,
                cap,
                "page %llu: not the data page for logical page %llu",
                (unsigned long long) id,
                (unsigned long long) lp);
        }
        (*count)++;
    }
    if (first >= n && m->tail != 0) {
        return fail(msg, cap, "meta: tail page %u with no live data pages", m->tail);
    }
    return 0;
}

int ml_check(const ml_log *l, ps_snapshot *snap, ml_stats *out, char *msg, usize msg_cap)
{
    *out = (ml_stats) {0};
    const ps_page *p;
    int rv = ps_snapshot_read(snap, l->meta_id, &p);
    if (rv != 0) {
        return rv;
    }
    meta m = {0};
    rv = meta_decode(p, &m);
    if (rv != 0) {
        rv = fail(msg, msg_cap, "page %u: not a log meta page", l->meta_id);
    }
    if (rv == 0) {
        rv = check_meta(l, p, &m, msg, msg_cap);
    }
    ps_snapshot_unpin(snap, l->meta_id);
    if (rv != 0) {
        return rv;
    }
    const rx_array index = index_of(l, &m);
    const rx_array pages = map_of(l, &m);
    const u64 first_page = m.first_off / l->data_size;
    rv = check_array(
        "index",
        &index,
        snap,
        m.first_seq,
        m.next_seq,
        &out->index_pages,
        msg,
        msg_cap);
    if (rv == 0) {
        rv = check_array(
            "page map",
            &pages,
            snap,
            first_page,
            page_count(l, m.end_off),
            &out->map_pages,
            msg,
            msg_cap);
    }
    if (rv == 0) {
        rv = check_offsets(&index, snap, &m, msg, msg_cap);
    }
    if (rv == 0) {
        rv = check_data(l, &pages, snap, &m, &out->data_pages, msg, msg_cap);
    }
    out->messages = m.next_seq - m.first_seq;
    out->bytes = m.end_off - m.first_off;
    out->readers = m.readers;
    return rv;
}
