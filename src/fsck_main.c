/*
 * fsck_main.c — entry point for the litefs-fsck binary
 *
 * Separated from main.c so the fsck binary doesn't pull in FUSE.
 *
 * Build:
 *   make fsck
 *
 * Usage:
 *   ./litefs-fsck <disk> <journal>         # read-only check
 *   ./litefs-fsck <disk> <journal> --fix   # repair
 */

#include "litefs.h"
#include <stdio.h>
#include <string.h>

/* defined in fsck.c */
int run_fsck(const char *disk_path, const char *journal_path, int fix);

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <disk> <journal> [--fix]\n", argv[0]);
        return 1;
    }

    int fix = (argc >= 4 && strcmp(argv[3], "--fix") == 0);
    return run_fsck(argv[1], argv[2], fix);
}
