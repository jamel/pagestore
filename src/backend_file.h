#pragma once

/*
 * The file backend keeps durable pages in data and recovers checkpoints
 * from journal. It holds LOCK while open to keep out a second opener.
 */

#include "pagestore/backend.h"
#include "pagestore/vfs.h"

/*
 * Copies *vfs and allocates a context owned by out->close. Disk access
 * under dir starts at out->open.
 */
int ps_file_backend_new(const char *dir, const ps_vfs *vfs, ps_backend *out);
