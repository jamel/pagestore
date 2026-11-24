#pragma once

/*
 * Little-endian loads and stores for integers inside byte buffers, such
 * as fields in a page payload or a log record. They work at any alignment.
 *
 *     u8 rec[12];
 *     ps_put_le32(rec, kind);
 *     ps_put_le64(rec + 4, lsn);
 *     ...
 *     const u64 lsn = ps_get_le64(rec + 4);
 */

#include "pagestore/compiler.h"
#include "pagestore/types.h"

/* Page header fields also go to disk in host byte order. */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
PS_STATIC_ASSERT(
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
    "pagestore requires a little-endian host");
#else
#error "pagestore cannot determine host byte order"
#endif

static inline void ps_put_le16(u8 *p, u16 v)
{
    PS_MEMCPY(p, &v, sizeof(v));
}

static inline void ps_put_le32(u8 *p, u32 v)
{
    PS_MEMCPY(p, &v, sizeof(v));
}

static inline void ps_put_le64(u8 *p, u64 v)
{
    PS_MEMCPY(p, &v, sizeof(v));
}

static inline u16 ps_get_le16(const u8 *p)
{
    u16 v;
    PS_MEMCPY(&v, p, sizeof(v));
    return v;
}

static inline u32 ps_get_le32(const u8 *p)
{
    u32 v;
    PS_MEMCPY(&v, p, sizeof(v));
    return v;
}

static inline u64 ps_get_le64(const u8 *p)
{
    u64 v;
    PS_MEMCPY(&v, p, sizeof(v));
    return v;
}
