/*
 * kv keeps a key-value map in a directory. It stores the map in the
 * B+tree from btree.h, on a PageStore store with a write-ahead log.
 *
 *     $ kv data put user:1 alice
 *     $ kv data put user:2 bob
 *     $ kv data get user:1
 *     alice
 *     $ kv data scan --prefix user:     # KEY<TAB>VALUE lines
 *     user:1  alice
 *     user:2  bob
 *     $ printf 'a\t1\nb\t2\n' | kv data load -
 *     loaded 2 entries
 *
 * Each change is one store operation with a logical log record. The same
 * function applies records live and during recovery.
 *
 *     put   | type = 2 | u16 klen | u16 vlen | key | value |
 *     del   | type = 3 | u16 klen | key |
 *     create| type = 1 |
 *
 * A put or del is durable once the command returns. A load flushes the log
 * every LOAD_FLUSH_EVERY lines, so a crash can lose the unflushed lines.
 *
 * Every run ends with a checkpoint unless --no-checkpoint is given, which
 * leaves the changes only in the log for the next open to replay.
 */

#include "btree.h"
#include "pagestore/pagestore.h"
#include "pagestore/wal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The store has no place to record the meta page ID, so kv uses a fixed one.
 * Page 0 holds the allocator bitmap, so a new store's first tree lands on
 * page 1. kv checks this when it creates the tree.
 */
#define KV_META_ID PS_LITERAL(ps_page_id, 1)

#define REC_CREATE 1u
#define REC_PUT 2u
#define REC_DELETE 3u
#define REC_PUT_HEADER 5u
#define REC_DELETE_HEADER 3u

#define LOAD_FLUSH_EVERY 1024u
#define WAL_SEGMENT_SIZE ((u64) 4 << 20)
/* Above the largest record, a put of a full entry in a 64 KiB page. */
#define WAL_MAX_RECORD (64u << 10)

#define EXIT_NOT_FOUND 1
#define EXIT_ERROR 2

/* Messages from the store at or below this level reach stderr. */
struct ps_log_ctx {
    int max_level;
};

typedef struct kv {
    ps_store *store;
    ps_wal *wal;
    bt_tree tree;
    bool tree_open;
    u64 next_lsn;
    u8 *rec; /* holds one encoded record */
    struct ps_log_ctx log;
} kv;

static void report(const char *what, int rv)
{
    fprintf(stderr, "kv: %s: %s\n", what, strerror(rv));
}

static void log_line(ps_log_ctx *ctx, int level, const char *msg)
{
    if (level <= ctx->max_level) {
        fprintf(stderr, "kv: store: %s\n", msg);
    }
}

/* ---- log records --------------------------------------------------------- */

static void put_le16(u8 *p, u32 v)
{
    p[0] = (u8) v;
    p[1] = (u8) (v >> 8);
}

static u32 get_le16(const u8 *p)
{
    return (u32) p[0] | (u32) p[1] << 8;
}

static usize rec_put(u8 *rec, const char *key, u32 klen, const char *val, u32 vlen)
{
    rec[0] = REC_PUT;
    put_le16(rec + 1, klen);
    put_le16(rec + 3, vlen);
    memcpy(rec + REC_PUT_HEADER, key, klen);
    memcpy(rec + REC_PUT_HEADER + klen, val, vlen);
    return REC_PUT_HEADER + klen + vlen;
}

static usize rec_delete(u8 *rec, const char *key, u32 klen)
{
    rec[0] = REC_DELETE;
    put_le16(rec + 1, klen);
    memcpy(rec + REC_DELETE_HEADER, key, klen);
    return REC_DELETE_HEADER + klen;
}

/* Applies one record inside the current operation, live or during replay. */
static int apply_record(bt_tree *t, const u8 *rec, usize len)
{
    if (len == 0) {
        return EBADMSG;
    }
    if (rec[0] == REC_CREATE && len == 1) {
        ps_page_id meta;
        const int rv = bt_create(t->store, &meta);
        if (rv == 0 && !ps_page_id_eq(meta, t->meta)) {
            fprintf(stderr, "kv: the new tree's meta page is %u, not %u\n", meta.raw, t->meta.raw);
            return EEXIST;
        }
        return rv;
    }
    if (rec[0] == REC_PUT && len >= REC_PUT_HEADER) {
        const u32 klen = get_le16(rec + 1);
        const u32 vlen = get_le16(rec + 3);
        if (len != REC_PUT_HEADER + klen + vlen) {
            return EBADMSG;
        }
        const u8 *key = rec + REC_PUT_HEADER;
        return bt_put(t, key, klen, key + klen, vlen);
    }
    if (rec[0] == REC_DELETE && len >= REC_DELETE_HEADER) {
        const u32 klen = get_le16(rec + 1);
        if (len != REC_DELETE_HEADER + klen) {
            return EBADMSG;
        }
        return bt_delete(t, rec + REC_DELETE_HEADER, klen);
    }
    return EBADMSG;
}

/* Holds the tree that recovery replays records into. */
struct ps_apply_ctx {
    bt_tree *tree;
};

static int replay(ps_apply_ctx *ctx, ps_store *s, u64 lsn, const void *rec, usize len)
{
    int rv = ps_op_begin(s, lsn);
    if (rv != 0) {
        return rv;
    }
    rv = apply_record(ctx->tree, rec, len);
    if (rv != 0) {
        ps_op_abort(s);
        return rv;
    }
    ps_op_commit(s);
    return 0;
}

/* ---- open, change and close ---------------------------------------------- */

/*
 * Runs rec as the next operation and logs it. With flush set, the record
 * is durable before the operation commits. An error leaves the store unchanged.
 */
static int kv_change(kv *db, const u8 *rec, usize len, bool flush)
{
    const u64 lsn = db->next_lsn;
    int rv = ps_op_begin(db->store, lsn);
    if (rv != 0) {
        return rv;
    }
    rv = apply_record(&db->tree, rec, len);
    if (rv == 0) {
        rv = ps_wal_append(db->wal, lsn, rec, len);
    }
    if (rv == 0 && flush) {
        rv = ps_wal_flush(db->wal, lsn);
    }
    if (rv != 0) {
        ps_op_abort(db->store);
        return rv;
    }
    ps_op_commit(db->store);
    db->next_lsn = lsn + 1;
    return 0;
}

/* Makes everything committed so far durable in the log. */
static int kv_flush(kv *db)
{
    return db->next_lsn > 1 ? ps_wal_flush(db->wal, db->next_lsn - 1) : 0;
}

/* Releases whatever kv_open set up. Checkpoints first if asked to. */
static int kv_close(kv *db, bool checkpoint)
{
    int rv = 0;
    if (db->store != NULL && checkpoint) {
        rv = ps_checkpoint(db->store);
        if (rv != 0) {
            report("checkpoint failed", rv);
        }
    }
    /* The store's checkpoint hooks use the log, so close the store first. */
    ps_close(db->store);
    ps_wal_close(db->wal);
    if (db->tree_open) {
        bt_close(&db->tree);
    }
    free(db->rec);
    return rv;
}

/* Opens the store, replays the log and creates the tree in a new store. */
static int kv_open(kv *db, const char *dir, bool create, bool verbose)
{
    memset(db, 0, sizeof(*db));
    db->log.max_level = verbose ? PS_LOG_INFO : PS_LOG_WARN;
    ps_config cfg;
    ps_config_init(&cfg);
    cfg.log = log_line;
    cfg.log_ctx = &db->log;
    int rv = ps_open(dir, &cfg, create ? PS_CREATE : 0, &db->store);
    if (rv == ENOENT && !create) {
        fprintf(stderr, "kv: no store in %s (put or load creates one)\n", dir);
        return rv;
    }
    if (rv != 0) {
        report("cannot open the store", rv);
        return rv;
    }
    ps_wal_config wcfg;
    ps_wal_config_init(&wcfg);
    wcfg.segment_size = WAL_SEGMENT_SIZE;
    wcfg.max_record_size = WAL_MAX_RECORD;
    rv = ps_wal_open(db->store, NULL, &wcfg, &db->wal);
    if (rv != 0) {
        report("cannot open the log", rv);
        return rv;
    }
    rv = bt_open(db->store, KV_META_ID, &db->tree);
    if (rv != 0) {
        report("cannot set up the tree", rv);
        return rv;
    }
    db->tree_open = true;
    db->rec = malloc(REC_PUT_HEADER + db->tree.max_entry);
    if (db->rec == NULL) {
        report("cannot allocate the record buffer", ENOMEM);
        return ENOMEM;
    }
    struct ps_apply_ctx actx = {&db->tree};
    rv = ps_recover(db->store, db->wal, replay, &actx);
    if (rv != 0) {
        report("cannot replay the log", rv);
        return rv;
    }
    db->next_lsn = ps_committed_lsn(db->store) + 1;
    if (ps_committed_lsn(db->store) == 0) {
        const u8 rec = REC_CREATE;
        rv = kv_change(db, &rec, 1, true);
        if (rv != 0) {
            report("cannot create the tree", rv);
            return rv;
        }
    }
    return 0;
}

static int open_snapshot(kv *db, ps_snapshot **snap)
{
    const int rv = ps_snapshot_open(db->store, snap);
    if (rv != 0) {
        report("cannot open a snapshot", rv);
    }
    return rv;
}

/* Reports why an entry can't be stored. */
static bool entry_fits(const kv *db, usize klen, usize vlen)
{
    if (klen > db->tree.max_key) {
        fprintf(stderr, "kv: keys are limited to %u bytes\n", db->tree.max_key);
        return false;
    }
    if (klen + vlen > db->tree.max_entry) {
        fprintf(
            stderr,
            "kv: a key and value are limited to %u bytes together\n",
            db->tree.max_entry);
        return false;
    }
    return true;
}

/* ---- commands --------------------------------------------------------------- */

static int cmd_put(kv *db, int argc, char **argv)
{
    if (argc != 2) {
        return -1;
    }
    const usize klen = strlen(argv[0]);
    const usize vlen = strlen(argv[1]);
    if (!entry_fits(db, klen, vlen)) {
        return EXIT_ERROR;
    }
    const usize len = rec_put(db->rec, argv[0], (u32) klen, argv[1], (u32) vlen);
    const int rv = kv_change(db, db->rec, len, true);
    if (rv != 0) {
        report("put failed", rv);
        return EXIT_ERROR;
    }
    return EXIT_SUCCESS;
}

static int cmd_get(kv *db, int argc, char **argv)
{
    if (argc != 1) {
        return -1;
    }
    const usize klen = strlen(argv[0]);
    ps_snapshot *snap;
    if (open_snapshot(db, &snap) != 0) {
        return EXIT_ERROR;
    }
    u8 *buf = db->rec; /* max_entry bytes hold any value */
    u32 vlen = 0;
    const int rv = bt_get(&db->tree, snap, argv[0], (u32) klen, buf, db->tree.max_entry, &vlen);
    ps_snapshot_close(snap);
    if (rv == ENOENT) {
        fprintf(stderr, "kv: not found\n");
        return EXIT_NOT_FOUND;
    }
    if (rv != 0) {
        report("get failed", rv);
        return EXIT_ERROR;
    }
    fwrite(buf, 1, vlen, stdout);
    putchar('\n');
    return EXIT_SUCCESS;
}

static int cmd_del(kv *db, int argc, char **argv)
{
    if (argc != 1) {
        return -1;
    }
    const usize klen = strlen(argv[0]);
    if (klen > db->tree.max_key) {
        fprintf(stderr, "kv: not found\n"); /* too long to have been stored */
        return EXIT_NOT_FOUND;
    }
    const usize len = rec_delete(db->rec, argv[0], (u32) klen);
    const int rv = kv_change(db, db->rec, len, true);
    if (rv == ENOENT) {
        fprintf(stderr, "kv: not found\n");
        return EXIT_NOT_FOUND;
    }
    if (rv != 0) {
        report("delete failed", rv);
        return EXIT_ERROR;
    }
    return EXIT_SUCCESS;
}

static bool has_prefix(const bt_item *it, const char *prefix, usize plen)
{
    return it->klen >= plen && memcmp(it->key, prefix, plen) == 0;
}

/* True if the item sorts at or after end. */
static bool past_end(const bt_item *it, const char *end, usize elen)
{
    const usize n = it->klen < elen ? it->klen : elen;
    const int c = memcmp(it->key, end, n);
    return c > 0 || (c == 0 && it->klen >= elen);
}

static int cmd_scan(kv *db, int argc, char **argv)
{
    const char *prefix = NULL;
    u64 limit = UINT64_MAX;
    const char *pos[2];
    int npos = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--prefix") == 0 && i + 1 < argc) {
            prefix = argv[++i];
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            char *end;
            errno = 0;
            limit = strtoull(argv[++i], &end, 10);
            if (errno != 0 || *end != '\0' || argv[i][0] == '\0' || argv[i][0] == '-') {
                return -1;
            }
        } else if (argv[i][0] != '-' && npos < 2) {
            pos[npos++] = argv[i];
        } else {
            return -1;
        }
    }
    if (prefix != NULL && npos > 0) {
        return -1;
    }
    const char *start = prefix != NULL ? prefix : npos > 0 ? pos[0] : "";
    const char *end = npos > 1 ? pos[1] : NULL;
    const usize slen = strlen(start);
    const usize elen = end != NULL ? strlen(end) : 0;
    const usize plen = prefix != NULL ? strlen(prefix) : 0;

    ps_snapshot *snap;
    if (open_snapshot(db, &snap) != 0) {
        return EXIT_ERROR;
    }
    bt_cursor c;
    bt_item it;
    u64 shown = 0;
    int rv = bt_cursor_open(&c, &db->tree, snap, start, (u32) slen);
    while (rv == 0 && shown < limit && (rv = bt_cursor_next(&c, &it)) == 0) {
        if (prefix != NULL && !has_prefix(&it, prefix, plen)) {
            break;
        }
        if (end != NULL && past_end(&it, end, elen)) {
            break;
        }
        fwrite(it.key, 1, it.klen, stdout);
        putchar('\t');
        fwrite(it.val, 1, it.vlen, stdout);
        putchar('\n');
        shown++;
    }
    bt_cursor_close(&c);
    ps_snapshot_close(snap);
    if (rv != 0 && rv != ENOENT) {
        report("scan failed", rv);
        return EXIT_ERROR;
    }
    return EXIT_SUCCESS;
}

/* Adds KEY<TAB>VALUE lines. Stops at the first bad line, keeping the lines before it. */
static int cmd_load(kv *db, int argc, char **argv)
{
    if (argc != 1) {
        return -1;
    }
    const bool use_stdin = strcmp(argv[0], "-") == 0;
    FILE *f = use_stdin ? stdin : fopen(argv[0], "r");
    if (f == NULL) {
        const int rv = errno;
        report(argv[0], rv);
        return EXIT_ERROR;
    }
    char *line = NULL;
    usize cap = 0;
    u64 lineno = 0;
    u64 loaded = 0;
    int status = EXIT_SUCCESS;
    for (;;) {
        const isize got = getline(&line, &cap, f);
        if (got < 0) {
            break;
        }
        lineno++;
        usize len = (usize) got;
        if (len > 0 && line[len - 1] == '\n') {
            len--;
        }
        if (len > 0 && line[len - 1] == '\r') {
            len--;
        }
        if (len == 0) {
            continue;
        }
        const char *tab = memchr(line, '\t', len);
        if (tab == NULL) {
            fprintf(stderr, "kv: line %llu: expected KEY<TAB>VALUE\n", (unsigned long long) lineno);
            status = EXIT_ERROR;
            break;
        }
        const usize klen = (usize) (tab - line);
        const usize vlen = len - klen - 1;
        if (!entry_fits(db, klen, vlen)) {
            fprintf(stderr, "kv: line %llu is too long\n", (unsigned long long) lineno);
            status = EXIT_ERROR;
            break;
        }
        const usize rlen = rec_put(db->rec, line, (u32) klen, tab + 1, (u32) vlen);
        int rv = kv_change(db, db->rec, rlen, false);
        if (rv == 0 && ++loaded % LOAD_FLUSH_EVERY == 0) {
            rv = kv_flush(db);
        }
        if (rv != 0) {
            fprintf(stderr, "kv: line %llu: %s\n", (unsigned long long) lineno, strerror(rv));
            status = EXIT_ERROR;
            break;
        }
    }
    if (ferror(f)) {
        report("read failed", EIO);
        status = EXIT_ERROR;
    }
    free(line);
    if (!use_stdin) {
        fclose(f);
    }
    const int rv = kv_flush(db);
    if (rv != 0) {
        report("log flush failed", rv);
        return EXIT_ERROR;
    }
    printf("loaded %llu %s\n", (unsigned long long) loaded, loaded == 1 ? "entry" : "entries");
    return status;
}

static unsigned fill_pct(u64 bytes, u64 pages, u32 capacity)
{
    return pages == 0 ? 0 : (unsigned) (bytes * 100 / (pages * capacity));
}

static int cmd_stats(kv *db, int argc, char **argv)
{
    PS_UNUSED(argv);
    if (argc != 0) {
        return -1;
    }
    ps_snapshot *snap;
    if (open_snapshot(db, &snap) != 0) {
        return EXIT_ERROR;
    }
    bt_stats bs;
    char msg[256];
    const int rv = bt_check(&db->tree, snap, &bs, msg, sizeof(msg));
    ps_snapshot_close(snap);
    if (rv != 0) {
        report(rv == EBADMSG ? msg : "cannot walk the tree", rv);
        return EXIT_ERROR;
    }
    struct ps_stats ss;
    ps_stats(db->store, &ss);
    const u32 cap = bs.node_capacity;
    printf("keys            %llu\n", (unsigned long long) bs.keys);
    printf("height          %u\n", bs.height);
    printf(
        "leaf pages      %llu, %u%% full\n",
        (unsigned long long) bs.leaf_pages,
        fill_pct(bs.leaf_bytes, bs.leaf_pages, cap));
    printf(
        "inner pages     %llu, %u%% full\n",
        (unsigned long long) bs.inner_pages,
        fill_pct(bs.inner_bytes, bs.inner_pages, cap));
    printf(
        "store pages     %llu, %u bytes each\n",
        (unsigned long long) ss.allocated_pages,
        ss.page_size);
    printf(
        "cache           %llu hits, %llu misses\n",
        (unsigned long long) ss.cache_hits,
        (unsigned long long) ss.cache_misses);
    printf("committed LSN   %llu\n", (unsigned long long) ss.committed_lsn);
    printf("durable LSN     %llu\n", (unsigned long long) ss.durable_lsn);
    return EXIT_SUCCESS;
}

static int cmd_check(kv *db, int argc, char **argv)
{
    PS_UNUSED(argv);
    if (argc != 0) {
        return -1;
    }
    ps_snapshot *snap;
    if (open_snapshot(db, &snap) != 0) {
        return EXIT_ERROR;
    }
    bt_stats bs;
    char msg[256];
    const int rv = bt_check(&db->tree, snap, &bs, msg, sizeof(msg));
    ps_snapshot_close(snap);
    if (rv != 0) {
        report(rv == EBADMSG ? msg : "cannot walk the tree", rv);
        return EXIT_ERROR;
    }
    /* Every allocated page should be the meta page or a tree node. */
    struct ps_stats ss;
    ps_stats(db->store, &ss);
    const u64 pages = 1 + bs.leaf_pages + bs.inner_pages;
    if (ss.allocated_pages != pages) {
        fprintf(
            stderr,
            "kv: the store has %llu pages allocated but the tree uses %llu\n",
            (unsigned long long) ss.allocated_pages,
            (unsigned long long) pages);
        return EXIT_ERROR;
    }
    printf(
        "ok: %llu keys in %llu pages\n",
        (unsigned long long) bs.keys,
        (unsigned long long) pages);
    return EXIT_SUCCESS;
}

/* ---- main ----------------------------------------------------------------------- */

/* Returns an exit status, or -1 for bad arguments. */
typedef int (*kv_cmd_fn)(kv *db, int argc, char **argv);

typedef struct kv_cmd {
    const char *name;
    kv_cmd_fn fn;
    bool creates; /* may create a new store */
} kv_cmd;

static const kv_cmd commands[] = {
    {"put", cmd_put, true},
    {"get", cmd_get, false},
    {"del", cmd_del, false},
    {"scan", cmd_scan, false},
    {"load", cmd_load, true},
    {"stats", cmd_stats, false},
    {"check", cmd_check, false},
};

static void usage(FILE *out)
{
    fputs(
        "usage: kv [-v] [--no-checkpoint] DIR COMMAND [ARGS]\n"
        "\n"
        "Commands:\n"
        "  put KEY VALUE        store VALUE under KEY, replacing any old value\n"
        "  get KEY              print the value stored under KEY\n"
        "  del KEY              delete KEY\n"
        "  scan [START [END]]   print entries from START up to, but not including, END\n"
        "  scan --prefix P      print entries whose keys start with P\n"
        "  load FILE            add KEY<TAB>VALUE lines from FILE, or - for stdin\n"
        "  stats                print tree and store statistics\n"
        "  check                verify the tree and look for leaked pages\n"
        "\n"
        "scan also takes --limit N. put and load create the store if needed.\n"
        "\n"
        "Options:\n"
        "  -v, --verbose        print the store's informational messages\n"
        "  --no-checkpoint      skip the final checkpoint and leave this run's\n"
        "                       changes in the log, as a crash would\n"
        "\n"
        "get and del exit with status 1 when KEY is missing. Errors exit with 2.\n",
        out);
}

int main(int argc, char **argv)
{
    bool verbose = false;
    bool checkpoint = true;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--no-checkpoint") == 0) {
            checkpoint = false;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return EXIT_SUCCESS;
        } else {
            usage(stderr);
            return EXIT_ERROR;
        }
    }
    if (argc - i < 2) {
        usage(stderr);
        return EXIT_ERROR;
    }
    const char *dir = argv[i];
    const char *name = argv[i + 1];
    const kv_cmd *cmd = NULL;
    for (usize j = 0; j < sizeof(commands) / sizeof(commands[0]); j++) {
        if (strcmp(commands[j].name, name) == 0) {
            cmd = &commands[j];
        }
    }
    if (cmd == NULL) {
        fprintf(stderr, "kv: unknown command '%s'\n", name);
        usage(stderr);
        return EXIT_ERROR;
    }

    kv db;
    if (kv_open(&db, dir, cmd->creates, verbose) != 0) {
        kv_close(&db, false);
        return EXIT_ERROR;
    }
    int status = cmd->fn(&db, argc - i - 2, argv + i + 2);
    if (status < 0) {
        usage(stderr);
        status = EXIT_ERROR;
    }
    if (kv_close(&db, checkpoint) != 0) {
        status = EXIT_ERROR;
    }
    return status;
}
