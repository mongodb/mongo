#!/bin/sh

# motivated by issue #3523

datagen > file
mkdir out
chmod 000 out

zstd file -q --trace-file-stat -o out/file.zst
zstd -tq out/file.zst

chmod 777 out
