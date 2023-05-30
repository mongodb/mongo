#!/bin/sh

zstdname=$(basename $0)

if [ -z "$EXEC_PREFIX" ]; then
    "$ZSTD_SYMLINK_DIR/$zstdname" $@
else
    $EXEC_PREFIX "$ZSTD_SYMLINK_DIR/$zstdname" $@
fi
