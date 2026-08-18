#!/usr/bin/env bash
#
# End-to-end tests for the FAT32 utility.
#
# Each test drives the shell over a pipe against a throwaway image, then checks
# the output. Where possible the image itself is validated afterwards with
# fsck.vfat, so the tests confirm the on-disk result and not merely what the
# tool printed about it.
#
#   ./tests/run-tests.sh
#
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/bin/filesys"
WORK="$(mktemp -d)"
SEED="$WORK/seed.img"

PASS=0
FAIL=0

cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

ok()   { printf '  \033[32mPASS\033[0m %s\n' "$1"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; FAIL=$((FAIL+1)); }

# check <description> <expected-substring> <actual-output>
check() {
    if [[ "$3" == *"$2"* ]]; then ok "$1"; else
        bad "$1"
        printf '        expected to find: %s\n' "$2"
        printf '        in output:\n%s\n' "$(echo "$3" | sed 's/^/          /' | head -20)"
    fi
}

# check_absent <description> <unexpected-substring> <actual-output>
check_absent() {
    if [[ "$3" != *"$2"* ]]; then ok "$1"; else
        bad "$1"
        printf '        did not expect: %s\n' "$2"
    fi
}

# fresh <path> - copy of a clean volume
fresh() { cp "$SEED" "$1"; }

# run <image> <commands...> - feed commands to the shell, echo its output
run() {
    local img="$1"; shift
    printf '%s\n' "$@" 'exit' | timeout 20 "$BIN" "$img" 2>&1
}

# ---------------------------------------------------------------- setup ----

if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not built. Run 'make' first." >&2
    exit 1
fi

echo "Preparing a clean FAT32 volume..."
if command -v mkfs.vfat > /dev/null 2>&1; then
    dd if=/dev/zero of="$SEED" bs=1M count=34 status=none
    mkfs.vfat -F 32 -n FAT32DISK "$SEED" > /dev/null 2>&1
elif [[ -f "$ROOT/fat32.img.gz" ]]; then
    gunzip -c "$ROOT/fat32.img.gz" > "$SEED"
else
    echo "error: need mkfs.vfat or fat32.img.gz to build a test volume" >&2
    exit 1
fi

HAVE_FSCK=0
command -v fsck.vfat > /dev/null 2>&1 && HAVE_FSCK=1

echo
echo "Running tests..."

# ------------------------------------------------------------- the tests ----

# mount and report geometry
img="$WORK/info.img"; fresh "$img"
out="$(run "$img" 'info')"
check "info reports the root cluster"      "position of root cluster" "$out"
check "info reports bytes per sector: 512" "bytes per sector: 512"    "$out"

# end of input must terminate the shell, not spin forever
img="$WORK/eof.img"; fresh "$img"
echo 'info' | timeout 5 "$BIN" "$img" > /dev/null 2>&1
if [[ $? -eq 124 ]]; then bad "end of input exits (timed out)"; else ok "end of input exits"; fi

# create and list
img="$WORK/creat.img"; fresh "$img"
out="$(run "$img" 'creat NOTES.TXT' 'ls')"
check "creat reports success"  "File 'NOTES.TXT' created" "$out"
check "ls shows the new file"  "NOTES.TXT"                "$out"

# the volume label is not a file and must not be listed
check_absent "ls hides the volume label" "FAT32DISK" "$out"

# write, then reopen: data must survive, i.e. the first cluster is recorded
img="$WORK/rt.img"; fresh "$img"
run "$img" 'creat A.TXT' 'open A.TXT -rw' 'write A.TXT hello world' 'close 0' > /dev/null
out="$(run "$img" 'open A.TXT -r' 'read 0 32')"
check "written data survives a reopen" "hello world" "$out"
out="$(run "$img" 'size A.TXT')"
check "size reports the written length" "11 A.TXT" "$out"

# names must be stored in real 8.3 form: "HELLO   TXT", not "HELLO.TXT  "
img="$WORK/n83.img"; fresh "$img"
run "$img" 'creat HELLO.TXT' > /dev/null
if python3 - "$img" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
i = d.find(b'HELLO')
sys.exit(0 if i >= 0 and d[i:i+11] == b'HELLO   TXT' else 1)
PY
then ok "name stored in 8.3 form on disk"; else bad "name stored in 8.3 form on disk"; fi

# a read spanning a cluster boundary must return the right bytes
img="$WORK/span.img"; fresh "$img"
BIG="$(python3 -c "print('ABCDEFGHIJ' * 60)")"      # 600 bytes, > one cluster
run "$img" 'creat BIG.TXT' 'open BIG.TXT -rw' "write BIG.TXT $BIG" 'close 0' > /dev/null
out="$(run "$img" 'open BIG.TXT -r' 'read 0 600')"
if [[ "$out" == *"$BIG"* ]]; then ok "read across a cluster boundary"; else
    bad "read across a cluster boundary"
    printf '        got: %s...\n' "$(echo "$out" | tr -d '\n' | cut -c1-80)"
fi

# lseek then read returns the tail of the file
img="$WORK/seek.img"; fresh "$img"
run "$img" 'creat S.TXT' 'open S.TXT -rw' 'write S.TXT abcdefghij' 'close 0' > /dev/null
out="$(run "$img" 'open S.TXT -r' 'lseek 0 5' 'read 0 5')"
check "lseek positions the next read" "fghij" "$out"

# directories
img="$WORK/dir.img"; fresh "$img"
out="$(run "$img" 'mkdir DOCS' 'cd DOCS' 'creat IN.TXT' 'ls' 'cd ..' 'ls')"
check "mkdir creates a directory"     "Directory 'DOCS' created" "$out"
check "cd enters it and ls shows it"  "IN.TXT"                   "$out"
check "cd .. returns to root"         "DOCS"                     "$out"

# rmdir refuses a non-empty directory, accepts an empty one
img="$WORK/rmdir.img"; fresh "$img"
out="$(run "$img" 'mkdir D' 'cd D' 'creat F.TXT' 'cd ..' 'rmdir D')"
check "rmdir refuses a non-empty directory" "is not empty" "$out"
out="$(run "$img" 'cd D' 'rm F.TXT' 'cd ..' 'rmdir D')"
check "rmdir removes an empty directory"    "Directory 'D' removed" "$out"
out="$(run "$img" 'ls')"
check_absent "the directory is gone from ls" "D" "$out"

# copy and rename
img="$WORK/cpmv.img"; fresh "$img"
run "$img" 'creat SRC.TXT' 'open SRC.TXT -rw' 'write SRC.TXT payload' 'close 0' > /dev/null
out="$(run "$img" 'cp SRC.TXT DST.TXT' 'open DST.TXT -r' 'read 0 16')"
check "cp duplicates the contents" "payload" "$out"
# listing is checked in its own session: the "Moved 'SRC.TXT' to ..." notice
# mentions the old name, so scanning the whole session would match it.
out="$(run "$img" 'mv SRC.TXT MOVED.TXT')"
check "mv reports the rename" "Moved 'SRC.TXT' to 'MOVED.TXT'" "$out"
out="$(run "$img" 'ls')"
check "mv renames"          "MOVED.TXT" "$out"
check_absent "old name gone" "SRC.TXT"   "$out"

# deleting a file frees it
img="$WORK/rm.img"; fresh "$img"
out="$(run "$img" 'creat GONE.TXT' 'rm GONE.TXT')"
check "rm deletes the file" "File 'GONE.TXT' deleted" "$out"
out="$(run "$img" 'ls')"
check_absent "deleted file is not listed" "GONE.TXT" "$out"

# error handling
img="$WORK/err.img"; fresh "$img"
out="$(run "$img" 'cd NOPE' 'open NOPE.TXT -r' 'read 9 10' 'bogus')"
check "cd on a missing directory errors"  "not found"       "$out"
check "open on a missing file errors"     "not found"       "$out"
check "read on a closed fd errors"        "Invalid fd"      "$out"
check "unknown commands are reported"     "Unknown command" "$out"

# the image must still be a valid FAT32 volume after all of that
if [[ $HAVE_FSCK -eq 1 ]]; then
    img="$WORK/fsck.img"; fresh "$img"
    run "$img" 'creat A.TXT' 'open A.TXT -rw' 'write A.TXT some data' 'close 0' \
               'mkdir SUB' 'cd SUB' 'creat B.TXT' 'cd ..' 'cp A.TXT C.TXT' > /dev/null
    fout="$(fsck.vfat -n "$img" 2>&1)"
    check_absent "fsck: FAT copies agree"     "FATs differ"           "$fout"
    check_absent "fsck: no malformed names"   "Bad short file name"   "$fout"
    check_absent "fsck: no size/chain mismatch" "cluster chain length" "$fout"
    check_absent "fsck: no orphaned clusters" "Reclaimed"             "$fout"
else
    echo "  SKIP fsck checks (fsck.vfat not installed)"
fi

# ------------------------------------------------------------------ done ----

echo
echo "$PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
