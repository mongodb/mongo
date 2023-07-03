#!/bin/sh

set -e

. "$COMMON/platform.sh"

zstd < file -vv -19 -o file.19.zst
zstd -vv -l file.19.zst

zstd < file -vv -19 --long -o file.19.long.zst
zstd -vv -l file.19.long.zst
