#!/bin/sh

set -e

. "$COMMON/platform.sh"

echo "" > 1
echo "2" > 2
echo "23" > 3
echo "234" > 4
echo "some data" > file

println "+ passthrough enabled"

zstd file

# Test short files
zstd -dc --pass-through 1 2 3 4

# Test *cat symlinks
zstdcat file
"$ZSTD_SYMLINK_DIR/zcat" file
"$ZSTD_SYMLINK_DIR/gzcat" file

# Test multiple files with mix of compressed & not
zstdcat file file.zst
zstdcat file.zst file

# Test --pass-through
zstd -dc --pass-through file
zstd -d --pass-through file -o pass-through-file

# Test legacy implicit passthrough with -fc
zstd -dcf file
zstd -dcf file file.zst
zstd -df < file
zstd -dcf < file file.zst -
zstd -dcf < file.zst file -

$DIFF file pass-through-file

println "+ passthrough disabled"

# Test *cat
zstdcat --no-pass-through file && die "should fail"
"$ZSTD_SYMLINK_DIR/zcat" --no-pass-through file && die "should fail"
"$ZSTD_SYMLINK_DIR/gzcat" --no-pass-through file && die "should fail"
# Test zstd without implicit passthrough
zstd -d file -o no-pass-through-file && die "should fail"
zstd -d < file && die "should fail"

# Test legacy implicit passthrough with -fc
zstd --no-pass-through -dcf file && die "should fail"
zstd --no-pass-through -dcf file file.zst && die "should fail"
zstd --no-pass-through -df < file && die "should fail"
zstd --no-pass-through -dcf < file file.zst - && die "should fail"
zstd --no-pass-through -dcf < file.zst file - && die "should fail" ||:
