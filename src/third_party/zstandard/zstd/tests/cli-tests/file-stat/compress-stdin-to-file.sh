#!/bin/sh

set -e

datagen > file

zstd < file -q --trace-file-stat -o file.zst
zstd -tq file.zst
