/*
 * path.c — path resolution helpers
 *
 * path_resolve("/foo/bar/baz")  → inode number of "baz"
 * path_resolve_parent("/foo/bar/baz") → inode of "/foo/bar", name = "baz"
 */

#include "litefs.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>

int path_resolve(struct litefs_state *fs, const char *path, uint32_t *out_ino)
{
    if (strcmp(path, "/") == 0) {
        *out_ino = LITEFS_ROOT_INO;
        return 0;
    }

    char buf[4096];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    uint32_t cur = LITEFS_ROOT_INO;
    char *saveptr;
    char *tok = strtok_r(buf, "/", &saveptr);

    while (tok) {
        uint32_t next;
        int ret = dir_lookup(fs, cur, tok, &next);
        if (ret < 0) return ret;
        cur = next;
        tok = strtok_r(NULL, "/", &saveptr);
    }

    *out_ino = cur;
    return 0;
}

int path_resolve_parent(struct litefs_state *fs, const char *path,
                        uint32_t *out_parent_ino, char *out_name)
{
    /* Find last '/' */
    const char *slash = strrchr(path, '/');
    if (!slash) return -EINVAL;

    /* Copy parent path */
    char parent_path[4096];
    size_t plen = slash - path;
    if (plen == 0) {
        strcpy(parent_path, "/");
    } else {
        strncpy(parent_path, path, plen);
        parent_path[plen] = '\0';
    }

    strncpy(out_name, slash + 1, LITEFS_NAME_MAX);
    out_name[LITEFS_NAME_MAX] = '\0';

    return path_resolve(fs, parent_path, out_parent_ino);
}
