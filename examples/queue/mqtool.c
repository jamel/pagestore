/*
 * mq keeps a durable message queue in a directory, using mq.h. Each run
 * opens the queue, runs one command and closes it.
 *
 *     $ mq data sub billing
 *     $ mq data push hello world
 *     0
 *     1
 *     $ mq data read billing --ack       # SEQ<TAB>MESSAGE lines
 *     0       hello
 *     1       world
 *     $ mq data read billing             # acknowledged, so nothing new
 *     $ mq data bench --producers 4 --readers 2
 *
 * A read without --ack leaves the reader's position alone. bench runs
 * producer and reader threads in one process and checks that every reader
 * sees each producer's messages in order.
 *
 * Every run ends with a checkpoint unless --no-checkpoint is given, which
 * leaves the changes only in the WAL, as a crash would.
 */

#include "mq.h"
#include "msglog.h"

#include "pagestore/bytes.h"
#include "pagestore/pagestore.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EXIT_NOT_FOUND 1
#define EXIT_ERROR 2

#define LOAD_BATCH 256u

typedef struct tool {
    mq *q;
    mq_config cfg;
} tool;

static void report(const char *what, int rv)
{
    fprintf(stderr, "mq: %s: %s\n", what, strerror(rv));
}

static void log_line(ps_logger_ctx *ctx, ps_log_level level, const char *fmt, va_list ap)
{
    PS_UNUSED(ctx, level);
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    fprintf(stderr, "mq: store: %s\n", msg);
}

/* Parses a whole decimal number. */
static bool parse_u64(const char *s, u64 *out)
{
    if (s[0] < '0' || s[0] > '9') {
        return false;
    }
    char *end;
    errno = 0;
    *out = strtoull(s, &end, 10);
    return errno == 0 && *end == '\0';
}

static bool parse_u32(const char *s, u32 *out)
{
    u64 v;
    if (!parse_u64(s, &v) || v > UINT32_MAX) {
        return false;
    }
    *out = (u32) v;
    return true;
}

static f64 now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (f64) t.tv_sec + (f64) t.tv_nsec / 1e9;
}

static int open_snapshot(tool *t, ps_snapshot **snap)
{
    const int rv = ps_snapshot_open(mq_store(t->q), snap);
    if (rv != 0) {
        report("cannot open a snapshot", rv);
    }
    return rv;
}

/* ---- push and load -------------------------------------------------------- */

static int cmd_push(tool *t, int argc, char **argv)
{
    if (argc < 1) {
        return -1;
    }
    for (int i = 0; i < argc; i++) {
        u64 seq;
        const int rv = mq_push(t->q, argv[i], (u32) strlen(argv[i]), &seq);
        if (rv != 0) {
            report("push failed", rv);
            return EXIT_ERROR;
        }
        printf("%llu\n", seq);
    }
    return EXIT_SUCCESS;
}

/* Lines waiting to go out as one batch, packed end to end in buf. */
typedef struct batch {
    char *buf;
    usize len;
    usize cap;
    u32 lens[LOAD_BATCH];
    u32 n;
} batch;

static int batch_flush(tool *t, batch *b)
{
    if (b->n == 0) {
        return 0;
    }
    mq_msg msgs[LOAD_BATCH];
    usize at = 0;
    for (u32 i = 0; i < b->n; i++) {
        msgs[i] = (mq_msg) {b->buf + at, b->lens[i]};
        at += b->lens[i];
    }
    u64 first;
    const int rv = mq_push_batch(t->q, msgs, b->n, &first);
    b->n = 0;
    b->len = 0;
    return rv;
}

static int batch_add(tool *t, batch *b, const char *line, usize len)
{
    if (b->n == LOAD_BATCH || b->len + len > t->cfg.max_batch) {
        const int rv = batch_flush(t, b);
        if (rv != 0) {
            return rv;
        }
    }
    if (b->len + len > b->cap) {
        const usize cap = b->len + len > 2 * b->cap ? b->len + len : 2 * b->cap;
        char *p = realloc(b->buf, cap);
        if (p == NULL) {
            return ENOMEM;
        }
        b->buf = p;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, line, len);
    b->len += len;
    b->lens[b->n++] = (u32) len;
    return 0;
}

/* Appends each line as a message. Stops at the first error, keeping the batches before it. */
static int cmd_load(tool *t, int argc, char **argv)
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
    batch b = {0};
    char *line = NULL;
    usize cap = 0;
    u64 loaded = 0;
    int rv = 0;
    for (;;) {
        const isize got = getline(&line, &cap, f);
        if (got < 0) {
            break;
        }
        usize len = (usize) got;
        if (len > 0 && line[len - 1] == '\n') {
            len--;
        }
        if (len > t->cfg.max_batch) {
            fprintf(stderr, "mq: line %llu is too long\n", loaded + 1);
            rv = EMSGSIZE;
            break;
        }
        rv = batch_add(t, &b, line, len);
        if (rv != 0) {
            report("load failed", rv);
            break;
        }
        loaded++;
    }
    if (rv == 0 && ferror(f)) {
        report("read failed", EIO);
        rv = EIO;
    }
    if (rv == 0) {
        rv = batch_flush(t, &b);
        if (rv != 0) {
            report("load failed", rv);
        }
    }
    free(line);
    free(b.buf);
    if (!use_stdin) {
        fclose(f);
    }
    if (rv != 0) {
        return EXIT_ERROR;
    }
    printf("loaded %llu %s\n", loaded, loaded == 1 ? "message" : "messages");
    return EXIT_SUCCESS;
}

/* ---- readers -------------------------------------------------------------- */

static int cmd_sub(tool *t, int argc, char **argv)
{
    if (argc < 1 || argc > 2) {
        return -1;
    }
    u64 pos = MQ_OLDEST;
    if (argc == 2 && strcmp(argv[1], "latest") == 0) {
        pos = MQ_LATEST;
    } else if (argc == 2 && strcmp(argv[1], "oldest") != 0 && !parse_u64(argv[1], &pos)) {
        return -1;
    }
    const int rv = mq_subscribe(t->q, argv[0], pos);
    if (rv == EINVAL) {
        fprintf(stderr, "mq: reader names are 1 to %u bytes\n", MQ_NAME_MAX);
        return EXIT_ERROR;
    }
    if (rv != 0) {
        report(rv == EEXIST ? argv[0] : "subscribe failed", rv);
        return EXIT_ERROR;
    }
    return EXIT_SUCCESS;
}

static int cmd_unsub(tool *t, int argc, char **argv)
{
    if (argc != 1) {
        return -1;
    }
    const int rv = mq_unsubscribe(t->q, argv[0]);
    if (rv == ENOENT) {
        fprintf(stderr, "mq: no reader %s\n", argv[0]);
        return EXIT_NOT_FOUND;
    }
    if (rv != 0) {
        report("unsubscribe failed", rv);
        return EXIT_ERROR;
    }
    return EXIT_SUCCESS;
}

static int open_reader(tool *t, const char *name, mq_reader *r)
{
    const int rv = mq_reader_open(t->q, name, r);
    if (rv == ENOENT) {
        fprintf(stderr, "mq: no reader %s\n", name);
        return EXIT_NOT_FOUND;
    }
    if (rv != 0) {
        report("cannot open the reader", rv);
        return EXIT_ERROR;
    }
    return EXIT_SUCCESS;
}

static int cmd_read(tool *t, int argc, char **argv)
{
    if (argc < 1) {
        return -1;
    }
    u64 limit = UINT64_MAX;
    bool ack = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ack") == 0) {
            ack = true;
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &limit)) {
                return -1;
            }
        } else {
            return -1;
        }
    }
    mq_reader r;
    int status = open_reader(t, argv[0], &r);
    if (status != EXIT_SUCCESS) {
        return status;
    }
    u8 *buf = malloc(t->cfg.max_batch);
    if (buf == NULL) {
        report("cannot allocate the read buffer", ENOMEM);
        mq_reader_close(&r);
        return EXIT_ERROR;
    }
    u64 shown = 0;
    u64 last = 0;
    int rv = 0;
    mq_item it;
    while (shown < limit && (rv = mq_read(&r, buf, t->cfg.max_batch, 0, &it)) == 0) {
        if (it.missed > 0) {
            fprintf(
                stderr,
                "mq: %s missed %llu %s\n",
                argv[0],
                it.missed,
                it.missed == 1 ? "message" : "messages");
        }
        printf("%llu\t", it.seq);
        fwrite(buf, 1, it.len, stdout);
        putchar('\n');
        last = it.seq;
        shown++;
    }
    if (rv != 0 && rv != ETIMEDOUT) {
        report("read failed", rv);
        status = EXIT_ERROR;
    }
    if (ack && shown > 0) {
        rv = mq_ack(&r, last);
        if (rv != 0) {
            report("ack failed", rv);
            status = EXIT_ERROR;
        }
    }
    free(buf);
    mq_reader_close(&r);
    return status;
}

static int cmd_ack(tool *t, int argc, char **argv)
{
    u64 seq;
    if (argc != 2 || !parse_u64(argv[1], &seq)) {
        return -1;
    }
    mq_reader r;
    int status = open_reader(t, argv[0], &r);
    if (status != EXIT_SUCCESS) {
        return status;
    }
    const int rv = mq_ack(&r, seq);
    if (rv == EINVAL) {
        fprintf(stderr, "mq: no message %llu yet\n", seq);
        status = EXIT_ERROR;
    } else if (rv != 0) {
        report("ack failed", rv);
        status = EXIT_ERROR;
    }
    mq_reader_close(&r);
    return status;
}

/* Prints NAME<TAB>POSITION<TAB>BEHIND, where BEHIND counts newer messages. */
static int cmd_readers(tool *t, int argc, char **argv)
{
    PS_UNUSED(argv);
    if (argc != 0) {
        return -1;
    }
    ps_snapshot *snap;
    if (open_snapshot(t, &snap) != 0) {
        return EXIT_ERROR;
    }
    const ml_log *l = mq_log(t->q);
    ml_state st;
    int rv = ml_state_get(l, snap, &st);
    u32 slot = 0;
    char name[MQ_NAME_MAX + 1];
    u64 pos;
    while (rv == 0 && (rv = ml_reader_next(l, snap, &slot, name, &pos)) == 0) {
        printf("%s\t%llu\t%llu\n", name, pos, st.next_seq - pos);
    }
    ps_snapshot_close(snap);
    if (rv != ENOENT) {
        report("cannot list readers", rv);
        return EXIT_ERROR;
    }
    return EXIT_SUCCESS;
}

/* ---- stats and check ------------------------------------------------------ */

static int check_log(tool *t, ml_stats *ms)
{
    ps_snapshot *snap;
    if (open_snapshot(t, &snap) != 0) {
        return EXIT_ERROR;
    }
    char msg[256];
    const int rv = ml_check(mq_log(t->q), snap, ms, msg, sizeof(msg));
    ps_snapshot_close(snap);
    if (rv != 0) {
        report(rv == EBADMSG ? msg : "cannot walk the log", rv);
        return EXIT_ERROR;
    }
    return EXIT_SUCCESS;
}

static int cmd_stats(tool *t, int argc, char **argv)
{
    PS_UNUSED(argv);
    if (argc != 0) {
        return -1;
    }
    ml_stats ms;
    if (check_log(t, &ms) != EXIT_SUCCESS) {
        return EXIT_ERROR;
    }
    ps_snapshot *snap;
    if (open_snapshot(t, &snap) != 0) {
        return EXIT_ERROR;
    }
    ml_state st;
    const int rv = ml_state_get(mq_log(t->q), snap, &st);
    ps_snapshot_close(snap);
    if (rv != 0) {
        report("cannot read the log", rv);
        return EXIT_ERROR;
    }
    struct ps_stats ss;
    ps_stats(mq_store(t->q), &ss);
    printf("messages        %llu\n", ms.messages);
    if (ms.messages > 0) {
        printf("sequence        %llu to %llu\n", st.first_seq, st.next_seq - 1);
    }
    printf("cap             %llu\n", st.cap);
    printf("payload bytes   %llu\n", ms.bytes);
    printf("readers         %u\n", ms.readers);
    printf("data pages      %llu\n", ms.data_pages);
    printf("index pages     %llu\n", ms.index_pages);
    printf("page map pages  %llu\n", ms.map_pages);
    printf("store pages     %llu, %u bytes each\n", ss.allocated_pages, ss.page_size);
    printf("committed LSN   %llu\n", ss.committed_lsn.raw);
    printf("durable LSN     %llu\n", ss.durable_lsn.raw);
    return EXIT_SUCCESS;
}

static int cmd_check(tool *t, int argc, char **argv)
{
    PS_UNUSED(argv);
    if (argc != 0) {
        return -1;
    }
    ml_stats ms;
    if (check_log(t, &ms) != EXIT_SUCCESS) {
        return EXIT_ERROR;
    }
    /* Every allocated page should be the meta page or belong to the log. */
    struct ps_stats ss;
    ps_stats(mq_store(t->q), &ss);
    const u64 pages = 1 + ms.data_pages + ms.index_pages + ms.map_pages;
    if (ss.allocated_pages != pages) {
        fprintf(
            stderr,
            "mq: the store has %llu pages allocated but the log uses %llu\n",
            ss.allocated_pages,
            pages);
        return EXIT_ERROR;
    }
    printf("ok: %llu messages in %llu pages\n", ms.messages, pages);
    return EXIT_SUCCESS;
}

/* ---- bench ---------------------------------------------------------------- */

/*
 * A bench message starts with its producer and that producer's counter.
 * The rest is a pattern derived from both, so readers can check every byte.
 */
#define BENCH_HEADER 12u

typedef struct bench {
    tool *t;
    u32 producers;
    u32 readers;
    u64 messages;
    u32 size;
    u32 batch;
    u64 start; /* sequence number of the first bench message */
    f64 t0;
} bench;

typedef struct bench_producer {
    bench *b;
    pthread_t thread;
    u32 id;
    u64 count;
    int rv;
} bench_producer;

typedef struct bench_reader {
    bench *b;
    pthread_t thread;
    char name[MQ_NAME_MAX + 1];
    u64 delivered;
    u64 missed;
    f64 done_at;
    int rv;
    char error[128];
} bench_reader;

static u8 pattern(u32 producer, u64 counter, u32 i)
{
    return (u8) (producer * 131u + counter * 31u + i);
}

static void bench_fill(u8 *p, u32 size, u32 producer, u64 counter)
{
    ps_put_le32(p, producer);
    ps_put_le64(p + 4, counter);
    for (u32 i = BENCH_HEADER; i < size; i++) {
        p[i] = pattern(producer, counter, i);
    }
}

static void *bench_produce(void *arg)
{
    bench_producer *p = arg;
    const bench *b = p->b;
    const u32 n = b->batch;
    u8 *buf = malloc((usize) b->size * n);
    mq_msg msgs[LOAD_BATCH];
    if (buf == NULL) {
        p->rv = ENOMEM;
        return NULL;
    }
    u64 counter = 0;
    while (counter < p->count && p->rv == 0) {
        const u32 k = p->count - counter < n ? (u32) (p->count - counter) : n;
        for (u32 i = 0; i < k; i++) {
            bench_fill(buf + (usize) i * b->size, b->size, p->id, counter + i);
            msgs[i] = (mq_msg) {buf + (usize) i * b->size, b->size};
        }
        u64 first;
        p->rv = mq_push_batch(b->t->q, msgs, k, &first);
        counter += k;
    }
    free(buf);
    return NULL;
}

/* Checks one delivered message. last[p] is the next counter expected from producer p. */
static bool bench_verify(const bench *b, const u8 *msg, u32 len, u64 *next, char *why, usize cap)
{
    const u32 producer = ps_get_le32(msg);
    const u64 counter = ps_get_le64(msg + 4);
    if (len != b->size || producer >= b->producers) {
        snprintf(why, cap, "bad message header");
        return false;
    }
    if (counter < next[producer]) {
        snprintf(
            why,
            cap,
            "producer %u: message %llu after %llu",
            producer,
            counter,
            next[producer] - 1);
        return false;
    }
    for (u32 i = BENCH_HEADER; i < len; i++) {
        if (msg[i] != pattern(producer, counter, i)) {
            snprintf(why, cap, "producer %u: message %llu is damaged", producer, counter);
            return false;
        }
    }
    next[producer] = counter + 1;
    return true;
}

static void *bench_read(void *arg)
{
    bench_reader *r = arg;
    const bench *b = r->b;
    const u64 end = b->start + b->messages;
    u8 *buf = malloc(b->size);
    u64 *next = calloc(b->producers, sizeof(*next));
    mq_reader h;
    r->rv = buf == NULL || next == NULL ? ENOMEM : mq_reader_open(b->t->q, r->name, &h);
    if (r->rv != 0) {
        free(buf);
        free(next);
        return NULL;
    }
    u64 seen = b->start; /* every message below this is delivered or missed */
    mq_item it;
    while (seen < end && r->rv == 0) {
        r->rv = mq_read(&h, buf, b->size, 10000, &it);
        if (r->rv != 0) {
            break;
        }
        r->delivered++;
        r->missed += it.missed;
        seen = it.seq + 1;
        if (!bench_verify(b, buf, it.len, next, r->error, sizeof(r->error))) {
            r->rv = EBADMSG;
        } else if (seen == end || r->delivered % 1024 == 0) {
            r->rv = mq_ack(&h, it.seq);
        }
    }
    r->done_at = now_s() - b->t0;
    mq_reader_close(&h);
    free(buf);
    free(next);
    return NULL;
}

/* Parses bench options into b. Returns false for a bad option. */
static bool bench_args(bench *b, int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (i + 1 == argc) {
            return false;
        }
        const char *opt = argv[i];
        const char *val = argv[++i];
        bool ok;
        if (strcmp(opt, "--producers") == 0) {
            ok = parse_u32(val, &b->producers) && b->producers >= 1 && b->producers <= 256;
        } else if (strcmp(opt, "--readers") == 0) {
            ok = parse_u32(val, &b->readers) && b->readers <= 64;
        } else if (strcmp(opt, "--messages") == 0) {
            ok = parse_u64(val, &b->messages) && b->messages >= 1;
        } else if (strcmp(opt, "--size") == 0) {
            ok = parse_u32(val, &b->size) && b->size >= BENCH_HEADER;
        } else if (strcmp(opt, "--batch") == 0) {
            ok = parse_u32(val, &b->batch) && b->batch >= 1 && b->batch <= LOAD_BATCH;
        } else {
            ok = false;
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* Replaces any reader left by an earlier bench with one at the newest message. */
static int bench_subscribe(tool *t, bench_reader *rs, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        snprintf(rs[i].name, sizeof(rs[i].name), "bench-%u", i);
        int rv = mq_unsubscribe(t->q, rs[i].name);
        if (rv == 0 || rv == ENOENT) {
            rv = mq_subscribe(t->q, rs[i].name, MQ_LATEST);
        }
        if (rv != 0) {
            report("cannot add the bench readers", rv);
            return rv;
        }
    }
    return 0;
}

static int cmd_bench(tool *t, int argc, char **argv)
{
    bench b = {.t = t, .producers = 4, .readers = 2, .messages = 20000, .size = 100, .batch = 1};
    if (!bench_args(&b, argc, argv)) {
        return -1;
    }
    if ((u64) b.size * b.batch > t->cfg.max_batch) {
        fprintf(stderr, "mq: a batch is limited to %u bytes\n", t->cfg.max_batch);
        return EXIT_ERROR;
    }
    bench_producer *ps = calloc(b.producers, sizeof(*ps));
    bench_reader *rs = calloc(b.readers > 0 ? b.readers : 1, sizeof(*rs));
    if (ps == NULL || rs == NULL || bench_subscribe(t, rs, b.readers) != 0) {
        free(ps);
        free(rs);
        return EXIT_ERROR;
    }
    ps_snapshot *snap;
    ml_state st;
    int rv = open_snapshot(t, &snap);
    if (rv == 0) {
        rv = ml_state_get(mq_log(t->q), snap, &st);
        ps_snapshot_close(snap);
    }
    if (rv != 0) {
        free(ps);
        free(rs);
        return EXIT_ERROR;
    }
    b.start = st.next_seq;
    b.t0 = now_s();
    for (u32 i = 0; i < b.readers; i++) {
        rs[i].b = &b;
        pthread_create(&rs[i].thread, NULL, bench_read, &rs[i]);
    }
    for (u32 i = 0; i < b.producers; i++) {
        ps[i] = (bench_producer) {.b = &b, .id = i, .count = b.messages / b.producers};
        if (i < b.messages % b.producers) {
            ps[i].count++;
        }
        pthread_create(&ps[i].thread, NULL, bench_produce, &ps[i]);
    }
    int status = EXIT_SUCCESS;
    for (u32 i = 0; i < b.producers; i++) {
        pthread_join(ps[i].thread, NULL);
        if (ps[i].rv != 0) {
            report("push failed", ps[i].rv);
            status = EXIT_ERROR;
        }
    }
    const f64 pushed = now_s() - b.t0;
    const f64 mib = (f64) b.messages * b.size / (1 << 20);
    printf(
        "pushed %llu messages of %u bytes in %.2f s: %.0f messages/s, %.1f MiB/s\n",
        b.messages,
        b.size,
        pushed,
        (f64) b.messages / pushed,
        mib / pushed);
    for (u32 i = 0; i < b.readers; i++) {
        pthread_join(rs[i].thread, NULL);
        bench_reader *r = &rs[i];
        if (r->rv != 0) {
            report(r->rv == EBADMSG ? r->error : "read failed", r->rv);
            status = EXIT_ERROR;
            continue;
        }
        printf(
            "%s read %llu messages, missed %llu, done at %.2f s\n",
            r->name,
            r->delivered,
            r->missed,
            r->done_at);
    }
    free(ps);
    free(rs);
    return status;
}

/* ---- main ----------------------------------------------------------------- */

/* Returns an exit status, or -1 for bad arguments. */
typedef int (*mq_cmd_fn)(tool *t, int argc, char **argv);

typedef struct mq_cmd {
    const char *name;
    mq_cmd_fn fn;
    bool creates; /* may create a new queue */
} mq_cmd;

static const mq_cmd commands[] = {
    {"push", cmd_push, true},
    {"load", cmd_load, true},
    {"sub", cmd_sub, true},
    {"unsub", cmd_unsub, false},
    {"read", cmd_read, false},
    {"ack", cmd_ack, false},
    {"readers", cmd_readers, false},
    {"stats", cmd_stats, false},
    {"check", cmd_check, false},
    {"bench", cmd_bench, true},
};

static void usage(FILE *out)
{
    fputs(
        "usage: mq [-v] [--no-checkpoint] [--cap N] DIR COMMAND [ARGS]\n"
        "\n"
        "Commands:\n"
        "  push MESSAGE...      append each MESSAGE and print its sequence number\n"
        "  load FILE            append each line of FILE, or - for stdin\n"
        "  sub NAME [START]     add reader NAME, starting at oldest (the default),\n"
        "                       latest or a sequence number\n"
        "  unsub NAME           remove reader NAME\n"
        "  read NAME            print SEQ<TAB>MESSAGE lines from NAME's position\n"
        "  ack NAME SEQ         acknowledge NAME's messages up to and including SEQ\n"
        "  readers              print NAME<TAB>POSITION<TAB>BEHIND lines\n"
        "  stats                print log and store statistics\n"
        "  check                verify the log and look for leaked pages\n"
        "  bench [OPTIONS]      run producer and reader threads against the queue\n"
        "\n"
        "read takes --limit N, and --ack to acknowledge what it printed.\n"
        "bench takes --producers N (4), --readers N (2), --messages N (20000),\n"
        "--size BYTES (100) and --batch N (1 message per push).\n"
        "push, load, sub and bench create the queue if needed.\n"
        "\n"
        "Options:\n"
        "  -v, --verbose        print the store's informational messages\n"
        "  --no-checkpoint      skip the final checkpoint and leave this run's\n"
        "                       changes in the log, as a crash would\n"
        "  --cap N              keep N messages in a new queue (1000000)\n"
        "\n"
        "Commands exit with status 1 when NAME is missing. Errors exit with 2.\n",
        out);
}

int main(int argc, char **argv)
{
    tool t = {0};
    mq_config_init(&t.cfg);
    ps_log_level max_level = PS_LOG_LEVEL_WARN;
    bool checkpoint = true;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            max_level = PS_LOG_LEVEL_INFO;
        } else if (strcmp(argv[i], "--no-checkpoint") == 0) {
            checkpoint = false;
        } else if (
            strcmp(argv[i], "--cap") == 0 && i + 1 < argc && parse_u64(argv[i + 1], &t.cfg.cap) &&
            t.cfg.cap > 0) {
            i++;
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
    const mq_cmd *cmd = NULL;
    for (usize j = 0; j < sizeof(commands) / sizeof(commands[0]); j++) {
        if (strcmp(commands[j].name, name) == 0) {
            cmd = &commands[j];
        }
    }
    if (cmd == NULL) {
        fprintf(stderr, "mq: unknown command '%s'\n", name);
        usage(stderr);
        return EXIT_ERROR;
    }

    t.cfg.logger = (ps_logger) {.max_level = max_level, .write = log_line};
    int rv = mq_open(dir, &t.cfg, cmd->creates ? PS_CREATE : 0, &t.q);
    if (rv == ENOENT && !cmd->creates) {
        fprintf(stderr, "mq: no queue in %s (push, load, sub or bench creates one)\n", dir);
        return EXIT_ERROR;
    }
    if (rv != 0) {
        report("cannot open the queue", rv);
        return EXIT_ERROR;
    }
    int status = cmd->fn(&t, argc - i - 2, argv + i + 2);
    if (status < 0) {
        usage(stderr);
        status = EXIT_ERROR;
    }
    if (checkpoint) {
        rv = mq_checkpoint(t.q);
        if (rv != 0) {
            report("checkpoint failed", rv);
            status = EXIT_ERROR;
        }
    }
    mq_close(t.q);
    return status;
}
