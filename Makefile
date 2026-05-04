# ─────────────────────────────────────────────────────────────────────────────
# LiteFS Makefile
#
# Targets:
#   all          — build litefs (FUSE daemon) + litefs-fsck + litefs_test
#   litefs       — main FUSE filesystem binary
#   litefs-fsck  — offline consistency checker (no FUSE dependency)
#   litefs_test  — unit/integration test runner (no FUSE dependency)
#   test         — build test_bin and run it
#   smoke        — mkfs + mount + basic ops + unmount
#   stress       — run the crash/stress simulation script
#   clean        — remove all build artifacts and temp disk images
# ─────────────────────────────────────────────────────────────────────────────

CC      := gcc
CFLAGS  := -Wall -Wextra -Wpedantic -std=c17 -g -O2 -D_FILE_OFFSET_BITS=64
LDFLAGS := -lpthread

INCDIR  := include
OBJDIR  := build
TESTDIR := tests

# ── pkg-config for FUSE (only needed by the main FUSE binary) ────────────────
FUSE_CFLAGS  := $(shell pkg-config --cflags fuse3 2>/dev/null || echo "-I/usr/include/fuse3")
FUSE_LDFLAGS := $(shell pkg-config --libs   fuse3 2>/dev/null || echo "-lfuse3")

# ── Source lists ─────────────────────────────────────────────────────────────
#
# Core FS objects — shared by all three binaries; compiled WITHOUT FUSE headers
# so they can link into the test and fsck binaries which have no fuse dependency.
CORE_OBJS := \
    $(OBJDIR)/block.o   \
    $(OBJDIR)/cache.o   \
    $(OBJDIR)/dir.o     \
    $(OBJDIR)/file.o    \
    $(OBJDIR)/fs.o      \
    $(OBJDIR)/inode.o   \
    $(OBJDIR)/journal.o \
    $(OBJDIR)/path.o

# Main FUSE binary
FUSE_OBJS := $(CORE_OBJS) \
    $(OBJDIR)/fuse_ops.o      \
    $(OBJDIR)/flush_thread.o  \
    $(OBJDIR)/fsck.o          \
    $(OBJDIR)/main.o

# fsck binary (no FUSE)
FSCK_OBJS := $(CORE_OBJS) \
    $(OBJDIR)/fsck.o     \
    $(OBJDIR)/fsck_main.o

# Unit test binary (no FUSE)
TEST_OBJS := $(CORE_OBJS) \
    $(OBJDIR)/fsck.o       \
    $(OBJDIR)/test_litefs.o

.PHONY: all clean test test_bin smoke stress mount_debug

# ── Default target ────────────────────────────────────────────────────────────
all: $(OBJDIR) litefs litefs-fsck litefs_test
	@echo ""
	@echo "  Built:"
	@echo "    ./litefs        — FUSE filesystem daemon"
	@echo "    ./litefs-fsck   — offline consistency checker"
	@echo "    ./litefs_test   — unit + integration tests"

$(OBJDIR):
	mkdir -p $(OBJDIR)

# ── Compile: FUSE-aware objects ───────────────────────────────────────────────
$(OBJDIR)/fuse_ops.o: src/fuse_ops.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -I$(INCDIR) -c $< -o $@

$(OBJDIR)/main.o: src/main.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -I$(INCDIR) -c $< -o $@

$(OBJDIR)/flush_thread.o: src/flush_thread.c
	$(CC) $(CFLAGS) -I$(INCDIR) -c $< -o $@

# ── Compile: non-FUSE core objects  (-DLITEFS_NO_FUSE skips fuse.h) ──────────
$(OBJDIR)/block.o:      src/block.c      ; $(CC) $(CFLAGS) -DLITEFS_NO_FUSE -I$(INCDIR) -c $< -o $@
$(OBJDIR)/cache.o:      src/cache.c      ; $(CC) $(CFLAGS) -DLITEFS_NO_FUSE -I$(INCDIR) -c $< -o $@
$(OBJDIR)/dir.o:        src/dir.c        ; $(CC) $(CFLAGS) -DLITEFS_NO_FUSE -I$(INCDIR) -c $< -o $@
$(OBJDIR)/file.o:       src/file.c       ; $(CC) $(CFLAGS) -DLITEFS_NO_FUSE -I$(INCDIR) -c $< -o $@
$(OBJDIR)/fs.o:         src/fs.c         ; $(CC) $(CFLAGS) -DLITEFS_NO_FUSE -I$(INCDIR) -c $< -o $@
$(OBJDIR)/inode.o:      src/inode.c      ; $(CC) $(CFLAGS) -DLITEFS_NO_FUSE -I$(INCDIR) -c $< -o $@
$(OBJDIR)/journal.o:    src/journal.c    ; $(CC) $(CFLAGS) -DLITEFS_NO_FUSE -I$(INCDIR) -c $< -o $@
$(OBJDIR)/path.o:       src/path.c       ; $(CC) $(CFLAGS) -DLITEFS_NO_FUSE -I$(INCDIR) -c $< -o $@
$(OBJDIR)/fsck.o:       src/fsck.c       ; $(CC) $(CFLAGS) -DLITEFS_NO_FUSE -I$(INCDIR) -c $< -o $@
$(OBJDIR)/fsck_main.o:  src/fsck_main.c  ; $(CC) $(CFLAGS) -DLITEFS_NO_FUSE -I$(INCDIR) -c $< -o $@
$(OBJDIR)/test_litefs.o: tests/test_litefs.c ; $(CC) $(CFLAGS) -DLITEFS_NO_FUSE -I$(INCDIR) -c $< -o $@

# ── Link ─────────────────────────────────────────────────────────────────────
litefs: $(OBJDIR) $(FUSE_OBJS)
	$(CC) $(FUSE_OBJS) -o $@ $(FUSE_LDFLAGS) $(LDFLAGS)
	@echo "Linked: ./litefs"

litefs-fsck: $(OBJDIR) $(FSCK_OBJS)
	$(CC) $(FSCK_OBJS) -o $@ $(LDFLAGS)
	@echo "Linked: ./litefs-fsck"

litefs_test: $(OBJDIR) $(TEST_OBJS)
	$(CC) $(TEST_OBJS) -o $@ $(LDFLAGS)
	@echo "Linked: ./litefs_test"

# ── Test targets ──────────────────────────────────────────────────────────────
test: litefs_test
	@echo "=== LiteFS unit + integration tests ==="
	./litefs_test
	@echo "========================================"

test_bin: litefs_test

# ── Smoke test (requires FUSE + fusermount3) ──────────────────────────────────
smoke: litefs
	@echo "=== LiteFS smoke test ==="
	./litefs mkfs /tmp/lfs_smoke.disk /tmp/lfs_smoke.journal 2048
	mkdir -p /tmp/lfs_smoke_mnt
	./litefs mount /tmp/lfs_smoke.disk /tmp/lfs_smoke.journal /tmp/lfs_smoke_mnt &
	sleep 1
	echo "Hello LiteFS" > /tmp/lfs_smoke_mnt/hello.txt
	cat /tmp/lfs_smoke_mnt/hello.txt
	mkdir -p /tmp/lfs_smoke_mnt/subdir
	echo "hostname" > /tmp/lfs_smoke_mnt/subdir/hostname
	ls -la /tmp/lfs_smoke_mnt/subdir
	ln -s /tmp/lfs_smoke_mnt/hello.txt /tmp/lfs_smoke_mnt/link.txt
	cat /tmp/lfs_smoke_mnt/link.txt
	fusermount3 -u /tmp/lfs_smoke_mnt 2>/dev/null || fusermount -u /tmp/lfs_smoke_mnt
	rm -f /tmp/lfs_smoke.disk /tmp/lfs_smoke.journal
	rmdir /tmp/lfs_smoke_mnt 2>/dev/null || true
	@echo "=== smoke test passed ==="

# ── Stress/crash test ────────────────────────────────────────────────────────
stress: litefs litefs-fsck
	@bash scripts/stress_test.sh

# ── Debug mount (foreground + verbose FUSE debug output) ────────────────────
mount_debug: litefs
	./litefs mkfs /tmp/lfs_debug.disk /tmp/lfs_debug.journal 4096
	mkdir -p /tmp/lfs_debug_mnt
	@echo "Mounting in foreground debug mode. Ctrl-C to stop."
	./litefs mount /tmp/lfs_debug.disk /tmp/lfs_debug.journal /tmp/lfs_debug_mnt -f -d

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(OBJDIR) litefs litefs-fsck litefs_test
	rm -f /tmp/lfs*.disk /tmp/lfs*.journal /tmp/litefs_test.*
	rm -rf /tmp/lfs_*_mnt /tmp/lfs_smoke_mnt /tmp/lfs_debug_mnt
	@echo "Clean complete"
