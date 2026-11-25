#include "test.h"

#include "pagestore/pagestore.h"
#include "pagestore/wal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keeps the last message and counts calls per level. */
struct ps_logger_ctx {
    u32 calls[PS_LOG_LEVEL_DEBUG + 1];
    ps_log_level last_level;
    char last[8192];
    usize last_len;
};

static void capture(ps_logger_ctx *ctx, ps_log_level level, const char *fmt, va_list ap)
{
    const int n = vsnprintf(ctx->last, sizeof(ctx->last), fmt, ap);
    T_CHECK(n >= 0);
    ctx->last_len = (usize) n;
    ctx->last_level = level;
    ctx->calls[level]++;
}

static ps_logger capture_logger(ps_logger_ctx *ctx, ps_log_level max_level)
{
    memset(ctx, 0, sizeof(*ctx));
    return (ps_logger) {.max_level = max_level, .ctx = ctx, .write = capture};
}

/* Counts its calls in *n, so a test can see whether an argument ran. */
static u32 count_eval(u32 *n, u32 v)
{
    (*n)++;
    return v;
}

static void test_zeroed_is_off(void)
{
    const ps_logger lg = {0};
    T_CHECK(!ps_log_enabled(&lg, PS_LOG_LEVEL_ERROR));
    T_CHECK(!ps_log_enabled(&lg, PS_LOG_LEVEL_DEBUG));
    u32 evaluated = 0;
    PS_LOG_ERROR(&lg, "%u", count_eval(&evaluated, 1));
    T_EQ(evaluated, 0);

    ps_config cfg;
    ps_config_init(&cfg);
    T_CHECK(cfg.logger.write == NULL && cfg.logger.ctx == NULL);
}

static void test_level_filter(void)
{
    ps_logger_ctx c;
    const ps_logger lg = capture_logger(&c, PS_LOG_LEVEL_WARN);
    PS_LOG_ERROR(&lg, "e");
    PS_LOG_WARN(&lg, "w");
    PS_LOG_INFO(&lg, "i");
    PS_LOG_DEBUG(&lg, "d");
    T_EQ(c.calls[PS_LOG_LEVEL_ERROR], 1);
    T_EQ(c.calls[PS_LOG_LEVEL_WARN], 1);
    T_EQ(c.calls[PS_LOG_LEVEL_INFO], 0);
    T_EQ(c.calls[PS_LOG_LEVEL_DEBUG], 0);
    T_EQ(strcmp(c.last, "w"), 0);

    /* A filtered call skips its arguments. */
    u32 evaluated = 0;
    PS_LOG_DEBUG(&lg, "%u", count_eval(&evaluated, 1));
    T_EQ(evaluated, 0);
    PS_LOG_WARN(&lg, "%u", count_eval(&evaluated, 1));
    T_EQ(evaluated, 1);
}

static const ps_logger *next_logger(const ps_logger *lg, u32 *n)
{
    (*n)++;
    return lg;
}

static void test_computed_level(void)
{
    ps_logger_ctx c;
    const ps_logger lg = capture_logger(&c, PS_LOG_LEVEL_DEBUG);
    u32 evaluated = 0;
    const bool fatal = false;
    PS_LOG(
        next_logger(&lg, &evaluated),
        fatal ? PS_LOG_LEVEL_ERROR : PS_LOG_LEVEL_WARN,
        "page %u: %s",
        7u,
        "x");
    T_EQ(evaluated, 1); /* lg once */
    T_EQ(c.last_level, PS_LOG_LEVEL_WARN);
    T_EQ(strcmp(c.last, "page 7: x"), 0);
}

/* The sink formats the message, so ps_log_write imposes no length limit. */
static void test_long_message(void)
{
    ps_logger_ctx *c = malloc(sizeof(*c));
    T_CHECK(c != NULL);
    const ps_logger lg = capture_logger(c, PS_LOG_LEVEL_ERROR);
    char path[5000];
    memset(path, 'p', sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    PS_LOG_ERROR(&lg, "WAL %s: segment %016llx has a bad header", path, 1ull);
    T_EQ(c->last_len, strlen(path) + strlen("WAL : segment 0000000000000001 has a bad header"));
    T_CHECK(strstr(c->last, "has a bad header") != NULL);
    free(c);
}

/* The WAL logs through the logger it copies from the store at open. */
static void test_wal_uses_store_logger(void)
{
    char *dir = t_tmpdir();
    ps_logger_ctx *c = malloc(sizeof(*c));
    T_CHECK(c != NULL);
    ps_config cfg;
    ps_config_init(&cfg);
    cfg.checkpoint_thread = false;
    cfg.logger = capture_logger(c, PS_LOG_LEVEL_INFO);
    ps_store *s = NULL;
    T_OK(ps_open(dir, &cfg, PS_CREATE, &s));

    ps_wal *w = NULL;
    T_OK(ps_wal_open(s, NULL, NULL, &w));
    ps_wal_close(w);
    c->last[0] = '\0';
    T_OK(ps_wal_open(s, NULL, NULL, &w));
    T_CHECK(strstr(c->last, "opened") != NULL);
    T_EQ(c->last_level, PS_LOG_LEVEL_INFO);
    ps_wal_close(w);

    ps_close(s);
    t_rmtree(dir);
    free(dir);
    free(c);
}

int main(int argc, char **argv)
{
    t_init(argc, argv);
    T_RUN(test_zeroed_is_off);
    T_RUN(test_level_filter);
    T_RUN(test_computed_level);
    T_RUN(test_long_message);
    T_RUN(test_wal_uses_store_logger);
    return t_done();
}
