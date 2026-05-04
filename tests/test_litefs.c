/*
 * test_litefs.c
 */

#include "litefs.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int run_fsck(
    const char *disk_path,
    const char *journal_path,
    int fix
);

static int g_test_num = 0;
static int g_failures = 0;

#define TEST(name, expr)                                           \
    do {                                                            \
        g_test_num++;                                               \
                                                                    \
        if (expr) {                                                 \
            printf("ok %d - %s\n", g_test_num, name);               \
        }                                                           \
        else {                                                      \
            printf(                                                 \
                "not ok %d - %s  [%s:%d]\n",                        \
                g_test_num,                                         \
                name,                                               \
                __FILE__,                                           \
                __LINE__                                            \
            );                                                      \
                                                                    \
            g_failures++;                                           \
        }                                                           \
    } while (0)

#define ASSERT_EQ(a, b) \
    TEST(#a " == " #b, (a) == (b))

#define ASSERT_NE(a, b) \
    TEST(#a " != " #b, (a) != (b))

#define ASSERT_GE(a, b) \
    TEST(#a " >= " #b, (a) >= (b))

#define ASSERT_OK(expr) \
    TEST(#expr " == 0", (expr) == 0)

#define ASSERT_ERR(expr) \
    TEST(#expr " < 0", (expr) < 0)

#define DISK_PATH    "/tmp/litefs_test.disk"
#define JOURNAL_PATH "/tmp/litefs_test.journal"

#define TEST_BLOCKS 512

static struct litefs_state* make_fs(void) {
    unlink(DISK_PATH);

    unlink(JOURNAL_PATH);

    int ret = litefs_init_disk(
        DISK_PATH,
        JOURNAL_PATH,
        TEST_BLOCKS
    );

    if (ret < 0) {
        fprintf(stderr, "mkfs failed\n");
        exit(1);
    }

    struct litefs_state *fs;

    ret = litefs_mount(
        DISK_PATH,
        JOURNAL_PATH,
        &fs
    );

    if (ret < 0) {
        fprintf(stderr, "mount failed\n");
        exit(1);
    }

    return fs;
}

static void teardown_fs(struct litefs_state *fs) {
    litefs_unmount(fs);

    unlink(DISK_PATH);

    unlink(JOURNAL_PATH);
}

static void test_superblock(void) {
    printf("\n# Superblock / mkfs\n");

    struct litefs_state *fs = make_fs();

    TEST(
        "magic is correct",
        fs->sb.s_magic == LITEFS_MAGIC
    );

    TEST(
        "block size is correct",
        fs->sb.s_block_size == LITEFS_BLOCK_SIZE
    );

    TEST(
        "free blocks matches total",
        fs->sb.s_free_blocks <= fs->sb.s_total_blocks
    );

    teardown_fs(fs);
}

static void test_block_allocator(void) {
    printf("\n# Block allocator\n");

    struct litefs_state *fs = make_fs();

    uint32_t before = fs->sb.s_free_blocks;

    uint32_t blk1;
    uint32_t blk2;

    ASSERT_OK(block_alloc(fs, &blk1));

    TEST(
        "free_blocks decremented after alloc",
        fs->sb.s_free_blocks == before - 1
    );

    ASSERT_OK(block_alloc(fs, &blk2));

    TEST(
        "two allocated blocks are distinct",
        blk1 != blk2
    );

    ASSERT_OK(block_free(fs, blk1));

    TEST(
        "free_blocks restored after free",
        fs->sb.s_free_blocks == before - 1
    );

    ASSERT_OK(block_free(fs, blk2));

    TEST(
        "free_blocks fully restored",
        fs->sb.s_free_blocks == before
    );

    teardown_fs(fs);
}

static void test_cache(void) {
    printf("\n# LRU cache\n");

    struct block_cache c;

    cache_init(&c);

    uint8_t wbuf[LITEFS_BLOCK_SIZE];
    uint8_t rbuf[LITEFS_BLOCK_SIZE];

    memset(wbuf, 0xAB, sizeof(wbuf));

    TEST(
        "cache miss on empty cache",
        cache_get(&c, 42, rbuf) == -1
    );

    ASSERT_OK(cache_put(&c, 42, wbuf, 0));

    TEST(
        "cache hit after put",
        cache_get(&c, 42, rbuf) == 0
    );

    TEST(
        "cache returns correct data",
        memcmp(rbuf, wbuf, LITEFS_BLOCK_SIZE) == 0
    );

    cache_invalidate(&c, 42);

    TEST(
        "cache miss after invalidate",
        cache_get(&c, 42, rbuf) == -1
    );
}

static void test_inodes(void) {
    printf("\n# Inode alloc/free\n");

    struct litefs_state *fs = make_fs();

    uint32_t before = fs->sb.s_free_inodes;

    uint32_t ino1;
    uint32_t ino2;

    ASSERT_OK(
        inode_alloc(
            fs,
            LITEFS_INODE_FILE,
            0644,
            &ino1
        )
    );

    TEST(
        "free_inodes decremented",
        fs->sb.s_free_inodes == before - 1
    );

    TEST(
        "inode_get returns non-null",
        inode_get(fs, ino1) != NULL
    );

    TEST(
        "inode type is FILE",
        inode_get(fs, ino1)->i_type ==
        LITEFS_INODE_FILE
    );

    ASSERT_OK(
        inode_alloc(
            fs,
            LITEFS_INODE_DIR,
            0755,
            &ino2
        )
    );

    TEST(
        "two inodes have distinct numbers",
        ino1 != ino2
    );

    ASSERT_OK(inode_free(fs, ino1));

    TEST(
        "free_inodes restored after free",
        fs->sb.s_free_inodes == before - 1
    );

    TEST(
        "inode_get returns NULL after free",
        inode_get(fs, ino1) == NULL
    );

    teardown_fs(fs);
}

static void test_file_rw(void) {
    printf("\n# File read/write/truncate\n");

    struct litefs_state *fs = make_fs();

    uint32_t ino;

    ASSERT_OK(
        inode_alloc(
            fs,
            LITEFS_INODE_FILE,
            0644,
            &ino
        )
    );

    struct litefs_inode *inode =
        inode_get(fs, ino);

    const char *msg = "Hello, LiteFS!";

    ssize_t n = file_write(
        fs,
        inode,
        msg,
        strlen(msg),
        0
    );

    TEST(
        "write returns correct byte count",
        n == (ssize_t)strlen(msg)
    );

    TEST(
        "inode size updated after write",
        inode->i_size == strlen(msg)
    );

    char rbuf[64];

    memset(rbuf, 0, sizeof(rbuf));

    n = file_read(
        fs,
        inode,
        rbuf,
        sizeof(rbuf),
        0
    );

    TEST(
        "read returns correct byte count",
        n == (ssize_t)strlen(msg)
    );

    TEST(
        "read returns correct content",
        strncmp(rbuf, msg, strlen(msg)) == 0
    );

    char big[LITEFS_BLOCK_SIZE * 2 + 100];

    memset(big, 'X', sizeof(big));

    n = file_write(
        fs,
        inode,
        big,
        sizeof(big),
        0
    );

    TEST(
        "large write succeeds",
        n == (ssize_t)sizeof(big)
    );

    char rbig[LITEFS_BLOCK_SIZE * 2 + 100];

    n = file_read(
        fs,
        inode,
        rbig,
        sizeof(rbig),
        0
    );

    TEST(
        "large read returns correct size",
        n == (ssize_t)sizeof(rbig)
    );

    TEST(
        "large read content matches",
        memcmp(rbig, big, sizeof(big)) == 0
    );

    ASSERT_OK(file_truncate(fs, inode, 10));

    TEST(
        "inode size after truncate",
        inode->i_size == 10
    );

    teardown_fs(fs);
}

static void test_dirs(void) {
    printf("\n# Directory operations\n");

    struct litefs_state *fs = make_fs();

    uint32_t found_ino;

    ASSERT_ERR(
        dir_lookup(
            fs,
            LITEFS_ROOT_INO,
            "nonexistent",
            &found_ino
        )
    );

    uint32_t file_ino;

    ASSERT_OK(
        inode_alloc(
            fs,
            LITEFS_INODE_FILE,
            0644,
            &file_ino
        )
    );

    ASSERT_OK(
        dir_add_entry(
            fs,
            LITEFS_ROOT_INO,
            "test.txt",
            file_ino,
            LITEFS_INODE_FILE
        )
    );

    ASSERT_OK(
        dir_lookup(
            fs,
            LITEFS_ROOT_INO,
            "test.txt",
            &found_ino
        )
    );

    TEST(
        "dir_lookup finds correct inode",
        found_ino == file_ino
    );

    teardown_fs(fs);
}

int main(void) {
    printf("TAP version 13\n");

    printf("# LiteFS test suite\n");

    test_superblock();
    test_block_allocator();
    test_cache();
    test_inodes();
    test_file_rw();
    test_dirs();

    printf("\n1..%d\n", g_test_num);

    if (g_failures == 0)
        printf("# All %d tests passed\n", g_test_num);
    else
        printf(
            "# %d/%d tests FAILED\n",
            g_failures,
            g_test_num
        );

    return g_failures > 0 ? 1 : 0;
}