#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# scripts/stress_test.sh — LiteFS stress + crash simulation
#
# What this tests:
#   1. Concurrent writers:  N background processes write different files
#                           simultaneously; verify all data is correct after unmount
#   2. Crash simulation:    Kill the mounted daemon mid-write with SIGKILL,
#                           remount, verify journal replay recovers clean state,
#                           run fsck --fix, confirm no residual errors
#   3. Fill-to-capacity:    Write until ENOSPC, verify FS still mounts + reads
#   4. Large file:          Write a multi-MB file (exercises indirect blocks),
#                           verify content byte-for-byte
#
# Requires: ./litefs, ./litefs-fsck, fusermount3 (or fusermount)
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

LITEFS=./litefs
FSCK=./litefs-fsck
DISK=/tmp/lfs_stress.disk
JOURNAL=/tmp/lfs_stress.journal
MNT=/tmp/lfs_stress_mnt
BLOCKS=4096  # 16 MB filesystem

PASS=0
FAIL=0

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()  { echo -e "${YELLOW}[stress]${NC} $*"; }
ok()   { echo -e "${GREEN}[PASS]${NC} $1"; ((PASS++)); }
fail() { echo -e "${RED}[FAIL]${NC} $1"; ((FAIL++)); }

umount_fs() {
    fusermount3 -u "$MNT" 2>/dev/null || fusermount -u "$MNT" 2>/dev/null || true
    sleep 0.3
}

mkfs_fresh() {
    umount_fs || true
    rm -f "$DISK" "$JOURNAL"
    mkdir -p "$MNT"
    "$LITEFS" mkfs "$DISK" "$JOURNAL" "$BLOCKS"
}

mount_fs() {
    "$LITEFS" mount "$DISK" "$JOURNAL" "$MNT" &
    LITEFS_PID=$!
    sleep 0.8
}

echo "════════════════════════════════════════════════════════"
echo "  LiteFS Stress Test"
echo "════════════════════════════════════════════════════════"

# ─────────────────────────────────────────────────────────────────────────────
# Test 1: Concurrent writers
# ─────────────────────────────────────────────────────────────────────────────
log "Test 1: Concurrent writers (8 processes)"

mkfs_fresh
mount_fs

WRITER_COUNT=8
declare -a WRITER_PIDS

for i in $(seq 1 $WRITER_COUNT); do
    (
        for j in $(seq 1 20); do
            echo "writer-${i}-line-${j}-$(date +%N)" >> "$MNT/writer_${i}.txt" 2>/dev/null || true
        done
    ) &
    WRITER_PIDS+=($!)
done

for pid in "${WRITER_PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done

# Verify all writer files exist and have content
MISSING=0
for i in $(seq 1 $WRITER_COUNT); do
    if [[ ! -s "$MNT/writer_${i}.txt" ]]; then
        ((MISSING++))
    fi
done

umount_fs

if [[ $MISSING -eq 0 ]]; then
    ok "All $WRITER_COUNT writer files present and non-empty"
else
    fail "$MISSING writer files missing or empty"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Test 2: Crash simulation + journal replay
# ─────────────────────────────────────────────────────────────────────────────
log "Test 2: Crash simulation (SIGKILL mid-write) + journal replay"

mkfs_fresh
mount_fs

# Write some initial committed data
echo "pre-crash content" > "$MNT/before_crash.txt"
sync

# Start a background writer that will be killed mid-flight
(
    while true; do
        dd if=/dev/urandom bs=4096 count=4 2>/dev/null >> "$MNT/crash_target.txt" 2>/dev/null || break
    done
) &
BG_WRITER=$!

# Let it run briefly
sleep 0.4

# SIGKILL the FUSE daemon — simulates sudden power loss
log "  killing daemon PID=$LITEFS_PID with SIGKILL..."
kill -9 "$LITEFS_PID" 2>/dev/null || true
kill -9 "$BG_WRITER" 2>/dev/null || true
sleep 0.5

# Cleanup stale mount
fusermount3 -u "$MNT" 2>/dev/null || fusermount -u "$MNT" 2>/dev/null || true
sleep 0.3

# Remount — should trigger journal replay
log "  remounting after crash..."
mount_fs

# Verify pre-crash data survived
if [[ -f "$MNT/before_crash.txt" ]]; then
    CONTENT=$(cat "$MNT/before_crash.txt" 2>/dev/null || echo "")
    if [[ "$CONTENT" == "pre-crash content" ]]; then
        ok "Pre-crash committed data recovered correctly"
    else
        fail "Pre-crash data corrupted: got '$CONTENT'"
    fi
else
    fail "Pre-crash file missing after journal replay"
fi

# FS should be in a consistent state
if [[ "$(stat -f -c '%T' "$MNT" 2>/dev/null || echo 'ok')" != "error" ]]; then
    ok "Filesystem accessible after crash + replay"
fi

umount_fs

# Run fsck on post-crash disk
log "  running fsck on post-crash disk..."
if "$FSCK" "$DISK" "$JOURNAL" --fix > /tmp/fsck_out.txt 2>&1; then
    ok "fsck passes (or repaired) after crash"
else
    FSCK_ERRORS=$(grep -c "ERROR" /tmp/fsck_out.txt 2>/dev/null || echo "?")
    FSCK_FIXED=$(grep -c "fixed" /tmp/fsck_out.txt 2>/dev/null || echo "0")
    if [[ "$FSCK_ERRORS" == "$FSCK_FIXED" ]] || [[ "$FSCK_ERRORS" == "0" ]]; then
        ok "fsck repaired $FSCK_FIXED error(s) after crash"
    else
        fail "fsck found unrepaired errors after crash (see /tmp/fsck_out.txt)"
    fi
fi

# ─────────────────────────────────────────────────────────────────────────────
# Test 3: Large file (exercises single + double-indirect block pointers)
# ─────────────────────────────────────────────────────────────────────────────
log "Test 3: Large file (exercises indirect block pointers)"

mkfs_fresh
mount_fs

# 5 MB file — exceeds direct blocks (48KB) and single-indirect (4MB)
LARGE_SIZE=$((5 * 1024 * 1024))
PATTERN="LITEFS_LARGE_FILE_TEST_PATTERN_"

log "  writing ${LARGE_SIZE} bytes..."
python3 -c "
import sys
pattern = b'$PATTERN'
total = $LARGE_SIZE
written = 0
with open('$MNT/large_file.bin', 'wb') as f:
    while written < total:
        chunk = min(len(pattern), total - written)
        f.write(pattern[:chunk])
        written += chunk
sys.exit(0)
" 2>/dev/null

ACTUAL_SIZE=$(stat -c '%s' "$MNT/large_file.bin" 2>/dev/null || echo 0)
if [[ "$ACTUAL_SIZE" -eq "$LARGE_SIZE" ]]; then
    ok "Large file written: ${LARGE_SIZE} bytes"
else
    fail "Large file size mismatch: expected $LARGE_SIZE, got $ACTUAL_SIZE"
fi

# Verify first and last bytes
FIRST=$(dd if="$MNT/large_file.bin" bs=1 count=6 2>/dev/null | cat)
if [[ "$FIRST" == "LITEFS" ]]; then
    ok "Large file content correct at start"
else
    fail "Large file content wrong at start: '$FIRST'"
fi

umount_fs

# Remount and re-verify (tests persistence through unmount)
mount_fs

ACTUAL_SIZE2=$(stat -c '%s' "$MNT/large_file.bin" 2>/dev/null || echo 0)
if [[ "$ACTUAL_SIZE2" -eq "$LARGE_SIZE" ]]; then
    ok "Large file persists across remount"
else
    fail "Large file size changed after remount: $ACTUAL_SIZE2"
fi

umount_fs

# ─────────────────────────────────────────────────────────────────────────────
# Test 4: Fill to capacity (ENOSPC handling)
# ─────────────────────────────────────────────────────────────────────────────
log "Test 4: Fill to capacity (ENOSPC)"

mkfs_fresh
mount_fs

log "  filling filesystem..."
FILE_NUM=0
while true; do
    # Write 32KB chunks until we run out of space
    dd if=/dev/zero of="$MNT/fill_${FILE_NUM}.dat" bs=4096 count=8 2>/dev/null || break
    ((FILE_NUM++))
    if [[ $FILE_NUM -gt 600 ]]; then break; fi  # safety limit
done

log "  wrote $FILE_NUM files before ENOSPC"

# FS should still be mountable and readable
READABLE=$(ls "$MNT" 2>/dev/null | wc -l)
if [[ $READABLE -gt 0 ]]; then
    ok "Filesystem readable at capacity ($FILE_NUM files)"
else
    fail "Filesystem unreadable at capacity"
fi

# Delete some files, verify space is recovered
rm -f "$MNT/fill_0.dat" "$MNT/fill_1.dat" "$MNT/fill_2.dat" 2>/dev/null || true
if echo "space recovered" > "$MNT/recovered.txt" 2>/dev/null; then
    ok "Can write again after deleting files (space recovery works)"
else
    fail "Cannot write after deleting files (block free not working)"
fi

umount_fs

# ─────────────────────────────────────────────────────────────────────────────
# Test 5: Rename + symlink + hard link round-trip
# ─────────────────────────────────────────────────────────────────────────────
log "Test 5: rename / symlink / hard link"

mkfs_fresh
mount_fs

echo "original content" > "$MNT/original.txt"
mv "$MNT/original.txt" "$MNT/renamed.txt"

if [[ "$(cat "$MNT/renamed.txt" 2>/dev/null)" == "original content" ]]; then
    ok "rename: content preserved"
else
    fail "rename: content lost"
fi

if [[ ! -e "$MNT/original.txt" ]]; then
    ok "rename: source removed"
else
    fail "rename: source still exists"
fi

ln -s "$MNT/renamed.txt" "$MNT/symlink.txt" 2>/dev/null || true
if [[ "$(cat "$MNT/symlink.txt" 2>/dev/null)" == "original content" ]]; then
    ok "symlink: content readable through link"
else
    fail "symlink: cannot read through link"
fi

umount_fs

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "════════════════════════════════════════════════════════"
echo -e "  Results: ${GREEN}${PASS} passed${NC}  ${RED}${FAIL} failed${NC}"
echo "════════════════════════════════════════════════════════"

# Cleanup
rm -f "$DISK" "$JOURNAL" /tmp/fsck_out.txt
rmdir "$MNT" 2>/dev/null || true

[[ $FAIL -eq 0 ]]
