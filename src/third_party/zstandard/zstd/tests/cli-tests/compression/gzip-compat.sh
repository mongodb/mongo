#!/bin/sh

set -e

# Uncomment the set -v line for debugging
# set -v

# Test gzip specific compression option
if $(command -v $ZSTD_SYMLINK_DIR/gzip); then
    $ZSTD_SYMLINK_DIR/gzip --fast file ; $ZSTD_SYMLINK_DIR/gzip -d file.gz
    $ZSTD_SYMLINK_DIR/gzip --best file ; $ZSTD_SYMLINK_DIR/gzip -d file.gz

    # Test -n / --no-name: do not embed original filename in archive
    $ZSTD_SYMLINK_DIR/gzip -n file           ; grep -qv file file.gz  ; $ZSTD_SYMLINK_DIR/gzip -d file.gz
    $ZSTD_SYMLINK_DIR/gzip --no-name file    ; grep -qv file file.gz  ; $ZSTD_SYMLINK_DIR/gzip -d file.gz
    $ZSTD_SYMLINK_DIR/gzip -c --no-name file | grep -qv file
fi
