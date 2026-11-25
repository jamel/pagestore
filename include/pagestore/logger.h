#pragma once

/*
 * Diagnostic messages from the store, the WAL and code built on them.
 * The PS_LOG_* macros check the level first, so a filtered call does not
 * evaluate its arguments. The sink can run on several threads at once,
 * sometimes with store locks held, so it must not call into the store or
 * the WAL.
 */

#include "pagestore/types.h" // IWYU pragma: export

#include <stdarg.h>

PS_EXTERN_C_BEGIN

typedef enum ps_log_level {
    PS_LOG_LEVEL_ERROR = 0,
    PS_LOG_LEVEL_WARN = 1,
    PS_LOG_LEVEL_INFO = 2,
    PS_LOG_LEVEL_DEBUG = 3,
} ps_log_level;

/* Defined by the sink. */
typedef struct ps_logger_ctx ps_logger_ctx;

/* A zeroed logger is off. */
typedef struct ps_logger {
    ps_log_level max_level; /* most verbose level written */
    ps_logger_ctx *ctx;
    /* Writes one message without a trailing newline. */
    void (*write)(ps_logger_ctx *ctx, ps_log_level level, const char *fmt, va_list ap);
} ps_logger;

static inline bool ps_log_enabled(const ps_logger *lg, ps_log_level level)
{
    return lg->write != NULL && level <= lg->max_level;
}

/* Calls lg->write without checking the level. Use the macros below. */
void ps_log_write(const ps_logger *lg, ps_log_level level, const char *fmt, ...) PS_PRINTF(3, 4);

/* Reads lg and level once. Use this form when the level is computed. */
#define PS_LOG(lg, level, ...) \
    do { \
        const ps_logger *ps_log_lg_ = (lg); \
        const ps_log_level ps_log_level_ = (level); \
        if (ps_log_enabled(ps_log_lg_, ps_log_level_)) { \
            ps_log_write(ps_log_lg_, ps_log_level_, __VA_ARGS__); \
        } \
    } while (0)

#define PS_LOG_ERROR(lg, ...) PS_LOG(lg, PS_LOG_LEVEL_ERROR, __VA_ARGS__)
#define PS_LOG_WARN(lg, ...) PS_LOG(lg, PS_LOG_LEVEL_WARN, __VA_ARGS__)
#define PS_LOG_INFO(lg, ...) PS_LOG(lg, PS_LOG_LEVEL_INFO, __VA_ARGS__)
#define PS_LOG_DEBUG(lg, ...) PS_LOG(lg, PS_LOG_LEVEL_DEBUG, __VA_ARGS__)

PS_EXTERN_C_END
