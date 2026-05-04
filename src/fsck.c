/*
 * fsck.c
 */

#include "litefs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct fsck_state {
    struct litefs_state *fs;

    int fix;

    int errors;

    int fixed;

    uint32_t block_refcount[LITEFS_MAX_BLOCKS];

    uint32_t inode_refcount[LITEFS_MAX_INODES];
};

#define FSCK_ERROR(fmt, ...)                                      \
    do {                                                          \
        fprintf(stderr, "[fsck ERROR] " fmt "\n", ##__VA_ARGS__); \
        state->errors++;                                          \
    } while (0)

#define FSCK_WARN(fmt, ...) \
    fprintf(stderr, "[fsck WARN]  " fmt "\n", ##__VA_ARGS__)

#define FSCK_INFO(fmt, ...) \
    printf("[fsck]       " fmt "\n", ##__VA_ARGS__)

static int pass1_superblock(
    struct fsck_state *state
) {
    struct litefs_superblock *sb =
        &state->fs->sb;

    printf("\n[fsck] Pass 1: Superblock\n");

    if (sb->s_magic != LITEFS_MAGIC) {
        FSCK_ERROR(
            "bad magic: 0x%X "
            "(expected 0x%X)",
            sb->s_magic,
            LITEFS_MAGIC
        );

        return -1;
    }

    if (sb->s_version != LITEFS_VERSION) {
        FSCK_WARN(
            "version mismatch: %u "
            "(expected %u)",
            sb->s_version,
            LITEFS_VERSION
        );
    }

    if (
        sb->s_block_size !=
        LITEFS_BLOCK_SIZE
    ) {
        FSCK_ERROR(
            "block size mismatch: %u "
            "(expected %u)",
            sb->s_block_size,
            LITEFS_BLOCK_SIZE
        );
    }

    if (
        sb->s_total_blocks >
        LITEFS_MAX_BLOCKS
    ) {
        FSCK_ERROR(
            "total_blocks %u exceeds "
            "max %u",
            sb->s_total_blocks,
            LITEFS_MAX_BLOCKS
        );
    }

    if (
        sb->s_total_inodes >
        LITEFS_MAX_INODES
    ) {
        FSCK_ERROR(
            "total_inodes %u exceeds "
            "max %u",
            sb->s_total_inodes,
            LITEFS_MAX_INODES
        );
    }

    if (
        sb->s_root_ino !=
        LITEFS_ROOT_INO
    ) {
        FSCK_ERROR(
            "root_ino is %u, "
            "expected %u",
            sb->s_root_ino,
            LITEFS_ROOT_INO
        );
    }

    if (sb->s_state != 0) {
        FSCK_WARN(
            "filesystem was not "
            "cleanly unmounted "
            "(s_state=%u)",
            sb->s_state
        );
    }

    if (state->errors == 0)
        FSCK_INFO("superblock OK");

    return 0;
}

static void account_inode_blocks(
    struct fsck_state *state,
    struct litefs_inode *inode
) {
    for (
        int i = 0;
        i < LITEFS_DIRECT_BLOCKS;
        i++
    ) {
        if (!inode->i_direct[i])
            continue;

        uint32_t rel =
            inode->i_direct[i] -
            DATA_OFFSET;

        if (rel < LITEFS_MAX_BLOCKS)
            state->block_refcount[rel]++;
    }

    if (inode->i_indirect) {
        uint32_t rel =
            inode->i_indirect -
            DATA_OFFSET;

        if (rel < LITEFS_MAX_BLOCKS)
            state->block_refcount[rel]++;

        uint32_t ptrs[
            LITEFS_BLOCK_SIZE /
            sizeof(uint32_t)
        ];

        if (
            block_read(
                state->fs,
                inode->i_indirect,
                ptrs
            ) == 0
        ) {
            for (
                size_t i = 0;
                i <
                LITEFS_BLOCK_SIZE /
                sizeof(uint32_t);
                i++
            ) {
                if (!ptrs[i])
                    continue;

                rel =
                    ptrs[i] -
                    DATA_OFFSET;

                if (rel < LITEFS_MAX_BLOCKS)
                    state
                        ->block_refcount[rel]++;
            }
        }
    }
}

static int pass2_inodes(
    struct fsck_state *state
) {
    printf("\n[fsck] Pass 2: Inode table\n");

    int found = 0;

    for (
        uint32_t i = 1;
        i < LITEFS_MAX_INODES;
        i++
    ) {
        struct litefs_inode *inode =
            &state->fs->inode_table[i];

        if (
            inode->i_type ==
            LITEFS_INODE_FREE
        ) {
            continue;
        }

        found++;

        if (
            inode->i_type !=
                LITEFS_INODE_FILE &&
            inode->i_type !=
                LITEFS_INODE_DIR &&
            inode->i_type !=
                LITEFS_INODE_SYMLINK
        ) {
            FSCK_ERROR(
                "inode %u: unknown "
                "type %u",
                i,
                inode->i_type
            );

            if (state->fix) {
                inode->i_type =
                    LITEFS_INODE_FREE;

                inode_flush(
                    state->fs,
                    i
                );

                state->fs
                    ->sb
                    .s_free_inodes++;

                FSCK_INFO(
                    "  fixed: marked "
                    "inode %u as free",
                    i
                );

                state->fixed++;
            }

            continue;
        }

        if (inode->i_ino != i) {
            FSCK_ERROR(
                "inode %u: i_ino field "
                "is %u",
                i,
                inode->i_ino
            );
        }

        uint64_t max_size =
            (uint64_t)inode->i_blocks *
            LITEFS_BLOCK_SIZE;

        if (
            inode->i_size >
            max_size +
            LITEFS_BLOCK_SIZE
        ) {
            FSCK_WARN(
                "inode %u: size=%llu "
                "but i_blocks=%u",
                i,
                (unsigned long long)
                    inode->i_size,
                inode->i_blocks
            );
        }

        if (inode->i_links == 0) {
            FSCK_ERROR(
                "inode %u: "
                "link count is 0",
                i
            );
        }

        account_inode_blocks(
            state,
            inode
        );
    }

    FSCK_INFO(
        "scanned %d active inodes",
        found
    );

    return 0;
}

static int pass3_blocks(
    struct fsck_state *state
) {
    printf(
        "\n[fsck] Pass 3: "
        "Block bitmap vs. "
        "inode references\n"
    );

    uint8_t *bm =
        state->fs->block_bitmap;

    int leaks = 0;

    int doubles = 0;

    for (
        uint32_t i = 0;
        i < LITEFS_MAX_BLOCKS;
        i++
    ) {
        int allocated =
            (bm[i / 8] >> (i % 8)) & 1;

        uint32_t refs =
            state->block_refcount[i];

        if (allocated && refs == 0) {
            FSCK_WARN(
                "block %u: allocated "
                "but not referenced",
                i
            );

            leaks++;

            if (state->fix) {
                bm[i / 8] &=
                    ~(1u << (i % 8));

                state->fs
                    ->sb
                    .s_free_blocks++;

                FSCK_INFO(
                    "  fixed: freed "
                    "block %u",
                    i
                );

                state->fixed++;
            }
        } else if (!allocated && refs > 0) {
            FSCK_ERROR(
                "block %u: referenced "
                "but bitmap clear",
                i
            );

            doubles++;

            if (state->fix) {
                bm[i / 8] |=
                    (1u << (i % 8));

                FSCK_INFO(
                    "  fixed: updated "
                    "bitmap for %u",
                    i
                );

                state->fixed++;
            }
        } else if (
            allocated &&
            refs > 1
        ) {
            FSCK_ERROR(
                "block %u: shared by "
                "%u inodes",
                i,
                refs
            );

            doubles++;
        }
    }

    if (
        leaks == 0 &&
        doubles == 0
    ) {
        FSCK_INFO("block bitmap OK");
    } else {
        FSCK_INFO(
            "%d leaked blocks, "
            "%d bitmap errors",
            leaks,
            doubles
        );
    }

    if (
        state->fix &&
        (leaks > 0 || doubles > 0)
    ) {
        bitmap_flush(state->fs);
    }

    return 0;
}

static int pass4_traverse_dir(
    struct fsck_state *state,
    uint32_t dir_ino,
    const char *path,
    int depth
) {
    if (depth > 64) {
        FSCK_ERROR(
            "directory depth limit "
            "reached at '%s'",
            path
        );

        return -1;
    }

    struct litefs_inode *dir =
        inode_get(
            state->fs,
            dir_ino
        );

    if (!dir) {
        FSCK_ERROR(
            "dir inode %u invalid",
            dir_ino
        );

        return -1;
    }

    if (
        dir->i_type !=
        LITEFS_INODE_DIR
    ) {
        FSCK_ERROR(
            "inode %u is not "
            "a directory",
            dir_ino
        );

        return -1;
    }

    uint8_t buf[LITEFS_BLOCK_SIZE];

    for (
        int b = 0;
        b < LITEFS_DIRECT_BLOCKS;
        b++
    ) {
        if (!dir->i_direct[b])
            continue;

        if (
            block_read(
                state->fs,
                dir->i_direct[b],
                buf
            ) < 0
        ) {
            FSCK_ERROR(
                "cannot read dir "
                "block %u",
                dir->i_direct[b]
            );

            continue;
        }

        struct litefs_dirent *ents =
            (struct litefs_dirent *)buf;

        uint32_t n =
            LITEFS_BLOCK_SIZE /
            sizeof(struct litefs_dirent);

        for (
            uint32_t i = 0;
            i < n;
            i++
        ) {
            if (!ents[i].d_ino)
                continue;

            if (
                strcmp(
                    ents[i].d_name,
                    "."
                ) == 0 ||
                strcmp(
                    ents[i].d_name,
                    ".."
                ) == 0
            ) {
                state
                    ->inode_refcount[dir_ino]++;

                continue;
            }

            uint32_t child_ino =
                ents[i].d_ino;

            if (
                child_ino >=
                LITEFS_MAX_INODES
            ) {
                FSCK_ERROR(
                    "invalid inode "
                    "%u in '%s/%s'",
                    child_ino,
                    path,
                    ents[i].d_name
                );

                continue;
            }

            struct litefs_inode *child =
                &state
                    ->fs
                    ->inode_table[child_ino];

            if (
                child->i_type ==
                LITEFS_INODE_FREE
            ) {
                FSCK_ERROR(
                    "'%s/%s' points "
                    "to free inode %u",
                    path,
                    ents[i].d_name,
                    child_ino
                );

                continue;
            }

            state
                ->inode_refcount[child_ino]++;

            if (
                child->i_type ==
                LITEFS_INODE_DIR
            ) {
                char child_path[4096];

                snprintf(
                    child_path,
                    sizeof(child_path),
                    "%s/%s",
                    path,
                    ents[i].d_name
                );

                pass4_traverse_dir(
                    state,
                    child_ino,
                    child_path,
                    depth + 1
                );
            }
        }
    }

    return 0;
}

static int pass4_directories(
    struct fsck_state *state
) {
    printf(
        "\n[fsck] Pass 4: "
        "Directory tree\n"
    );

    struct litefs_inode *root =
        inode_get(
            state->fs,
            LITEFS_ROOT_INO
        );

    if (!root) {
        FSCK_ERROR(
            "root inode missing"
        );

        return -1;
    }

    if (
        root->i_type !=
        LITEFS_INODE_DIR
    ) {
        FSCK_ERROR(
            "root inode is "
            "not a directory"
        );
    }

    state
        ->inode_refcount[
            LITEFS_ROOT_INO
        ] += 2;

    pass4_traverse_dir(
        state,
        LITEFS_ROOT_INO,
        "",
        0
    );

    if (state->errors == 0)
        FSCK_INFO("directory tree OK");

    return 0;
}

static int pass5_link_counts(
    struct fsck_state *state
) {
    printf(
        "\n[fsck] Pass 5: "
        "Link counts\n"
    );

    int mismatches = 0;

    for (
        uint32_t i = 1;
        i < LITEFS_MAX_INODES;
        i++
    ) {
        struct litefs_inode *inode =
            &state->fs->inode_table[i];

        if (
            inode->i_type ==
            LITEFS_INODE_FREE
        ) {
            continue;
        }

        uint32_t expected =
            state->inode_refcount[i];

        if (inode->i_links != expected) {
            FSCK_ERROR(
                "inode %u: "
                "i_links=%u expected=%u",
                i,
                inode->i_links,
                expected
            );

            mismatches++;

            if (state->fix) {
                inode->i_links =
                    expected;

                if (expected == 0) {
                    inode_free(
                        state->fs,
                        i
                    );

                    FSCK_INFO(
                        "  fixed: freed "
                        "inode %u",
                        i
                    );
                } else {
                    inode_flush(
                        state->fs,
                        i
                    );

                    FSCK_INFO(
                        "  fixed: updated "
                        "i_links for %u",
                        i
                    );
                }

                state->fixed++;
            }
        }
    }

    if (mismatches == 0)
        FSCK_INFO("link counts OK");

    return 0;
}

int run_fsck(
    const char *disk_path,
    const char *journal_path,
    int fix
) {
    printf(
        "LiteFS fsck — "
        "disk: %s  "
        "journal: %s  "
        "mode: %s\n",
        disk_path,
        journal_path,
        fix ? "REPAIR" : "READ-ONLY"
    );

    struct litefs_state *fs;

    if (
        litefs_mount(
            disk_path,
            journal_path,
            &fs
        ) < 0
    ) {
        fprintf(
            stderr,
            "fsck: failed to "
            "open filesystem\n"
        );

        return 1;
    }

    struct fsck_state state;

    memset(&state, 0, sizeof(state));

    state.fs = fs;

    state.fix = fix;

    int ret = 0;

    ret = pass1_superblock(&state);

    if (ret < 0)
        goto done;

    pass2_inodes(&state);

    pass3_blocks(&state);

    pass4_directories(&state);

    pass5_link_counts(&state);

done:
    printf(
        "\n[fsck] Summary: "
        "%d error(s) found",
        state.errors
    );

    if (fix)
        printf(
            ", %d fixed",
            state.fixed
        );

    printf("\n");

    if (
        fix &&
        state.fixed > 0
    ) {
        sb_flush(fs);

        bitmap_flush(fs);

        printf(
            "[fsck] changes "
            "written to disk\n"
        );
    }

    fs->sb.s_state = 0;

    sb_flush(fs);

    close(fs->disk_fd);

    close(fs->journal_fd);

    free(fs);

    return (
        state.errors >
        state.fixed
    ) ? 1 : 0;
}