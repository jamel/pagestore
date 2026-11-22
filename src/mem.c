#include "store.h"

/*
 * A frame request takes a free frame or fails with ENOBUFS.
 */
int ps_mem_frame(ps_store *s, bool writer, u32 *f)
{
    PS_UNUSED(writer);
    return ps_frames_alloc(s->frames, f);
}

void ps_mem_release(ps_store *s, u32 f)
{
    ps_frames_release(s->frames, f);
}
