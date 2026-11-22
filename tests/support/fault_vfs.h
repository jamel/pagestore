#pragma once

/*
 * Use absolute paths. The root survives every reboot, and rename works
 * within a directory. Missing 4 KiB blocks read as zeros.
 *
 * fsync makes a file's bytes and size durable. Its directory entry /d/f
 * needs a separate fsync_dir on /d.
 *
 * Writes, syncs, size changes and directory changes each use a fault
 * point. Open and lock use one only when they create a file. After a
 * crash, every VFS call except close returns EIO until reboot.
 *
 * Close all handles before destroying the VFS. list callbacks can call
 * back into the VFS.
 */

#include "pagestore/types.h"
#include "pagestore/vfs.h"

typedef struct fault_vfs fault_vfs;

typedef enum fault_mode {
    /* Every unsynced change is lost. */
    FAULT_DROP_UNSYNCED,
    /*
     * Replay unsynced changes in order. Each file change has equal odds
     * of being kept, dropped or torn. A torn write keeps a prefix in
     * multiples of 512 bytes, and a torn size change is dropped.
     * Each directory operation has a 50% chance of replaying.
     */
    FAULT_RANDOM_UNSYNCED
} fault_mode;

/* Use the same seed and call order to reproduce a random reboot. */
fault_vfs *fault_vfs_create(u64 seed);
void fault_vfs_destroy(fault_vfs *fv);
/* The vfs to hand to the code under test; valid until fault_vfs_destroy(). */
const ps_vfs *fault_vfs_get(fault_vfs *fv);

/*
 * Creates a durable fixture directory, or syncs an existing one, without
 * using a fault point. The parent must exist.
 */
void fault_vfs_mkdir_durable(fault_vfs *fv, const char *path);

/* Monotonic across reboots and frozen while crashed. */
u64 fault_vfs_points(fault_vfs *fv);
/* Use fault_vfs_points(fv) + 1 for the next point, or 0 to disarm. */
void fault_vfs_crash_at(fault_vfs *fv, u64 point);
bool fault_vfs_crashed(fault_vfs *fv);
/* Inject err at one point with no side effects. Pass 0 to disarm. */
void fault_vfs_fail_at(fault_vfs *fv, u64 point, int err);
/*
 * The limit counts logical bytes in linked or open files, including
 * preallocation and sparse holes. It survives reboot. Use UINT64_MAX
 * for unlimited space.
 */
void fault_vfs_enospc_after(fault_vfs *fv, u64 bytes);
/*
 * The surviving state becomes durable. Reboot clears injected faults
 * and locks. Close previous handles and open new ones for I/O.
 */
void fault_vfs_reboot(fault_vfs *fv, fault_mode mode);
/*
 * Flips bits in the durable image and in the current image if the byte
 * exists there. The path resolves in the current directory tree. Returns
 * EINVAL for an offset outside the durable image.
 */
int fault_vfs_corrupt(fault_vfs *fv, const char *path, u64 off, u8 xor_mask);

/*
 * Resolves path in the current directory tree and reads the durable
 * bytes. It can open a name even before that name is durable. A short
 * read means the durable end of the file.
 */
int fault_vfs_read_durable(
    fault_vfs *fv,
    const char *path,
    u64 off,
    void *buf,
    usize len,
    usize *got);
