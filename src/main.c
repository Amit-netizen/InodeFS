/*
 * main.c — LiteFS entry point
 *
 * Usage:
 *   litefs mkfs  <disk_file> <journal_file> [blocks]
 *   litefs mount <disk_file> <journal_file> <mountpoint> [fuse_options...]
 *
 * Example:
 *   ./litefs mkfs   /tmp/lfs.disk /tmp/lfs.journal 4096
 *   mkdir -p /tmp/lfs_mount
 *   ./litefs mount  /tmp/lfs.disk /tmp/lfs.journal /tmp/lfs_mount -f
 *   # -f = foreground (useful for debugging; remove for daemon mode)
 *
 *   # In another terminal:
 *   ls /tmp/lfs_mount
 *   echo "hello LiteFS" > /tmp/lfs_mount/test.txt
 *   cat /tmp/lfs_mount/test.txt
 *   mkdir /tmp/lfs_mount/mydir
 *   cp /etc/hostname /tmp/lfs_mount/mydir/
 *   ls -la /tmp/lfs_mount/mydir
 *   fusermount3 -u /tmp/lfs_mount    # unmount
 */

#define FUSE_USE_VERSION 31
#include <fuse.h>
#include "litefs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct fuse_operations litefs_ops;

static void usage(const char *prog)
{
    fprintf(stderr,
        "LiteFS — user-space filesystem with journaling and LRU block cache\n\n"
        "  mkfs:   %s mkfs  <disk> <journal> [total_data_blocks]\n"
        "  mount:  %s mount <disk> <journal> <mountpoint> [fuse_opts...]\n\n"
        "Examples:\n"
        "  %s mkfs  /tmp/lfs.disk /tmp/lfs.journal 4096\n"
        "  mkdir -p /tmp/mnt\n"
        "  %s mount /tmp/lfs.disk /tmp/lfs.journal /tmp/mnt -f\n",
        prog, prog, prog, prog);
}

int main(int argc, char *argv[])
{
    if (argc < 4) { usage(argv[0]); return 1; }

    const char *cmd          = argv[1];
    const char *disk_path    = argv[2];
    const char *journal_path = argv[3];

    /* ── mkfs mode ── */
    if (strcmp(cmd, "mkfs") == 0) {
        uint32_t blocks = (argc >= 5) ? (uint32_t)atoi(argv[4]) : 2048;
        return litefs_init_disk(disk_path, journal_path, blocks) < 0 ? 1 : 0;
    }

    /* ── mount mode ── */
    if (strcmp(cmd, "mount") == 0) {
        if (argc < 5) { usage(argv[0]); return 1; }
        const char *mountpoint = argv[4];

        struct litefs_state *fs;
        if (litefs_mount(disk_path, journal_path, &fs) < 0) {
            fprintf(stderr, "Failed to mount LiteFS\n");
            return 1;
        }

        /*
         * Build fuse argv:
         *   fuse_argv[0] = program name
         *   fuse_argv[1] = mountpoint
         *   fuse_argv[2..] = any extra fuse options from command line
         */
        int fuse_argc = 2 + (argc - 5);
        char **fuse_argv = calloc(fuse_argc + 1, sizeof(char *));
        fuse_argv[0] = argv[0];
        fuse_argv[1] = (char *)mountpoint;
        for (int i = 5; i < argc; i++)
            fuse_argv[2 + (i - 5)] = argv[i];

        printf("[main] Mounting LiteFS at %s\n", mountpoint);
        int ret = fuse_main(fuse_argc, fuse_argv, &litefs_ops, fs);
        free(fuse_argv);
        return ret;
    }

    usage(argv[0]);
    return 1;
}
