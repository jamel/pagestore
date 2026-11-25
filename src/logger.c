#include "pagestore/logger.h"

/* The PS_LOG_* macros in logger.h filter by level before calling this. */

void ps_log_write(const ps_logger *lg, ps_log_level level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    lg->write(lg->ctx, level, fmt, ap);
    va_end(ap);
}
