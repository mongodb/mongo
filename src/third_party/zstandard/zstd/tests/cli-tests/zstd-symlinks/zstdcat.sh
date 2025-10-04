#!/bin/sh
set -e

# Test zstdcat symlink in bin/
zstdcat hello.zst
zstdcat hello.zst world
zstdcat hello world.zst
zstdcat hello.zst world.zst

# Test local zstdcat symlink
ln -s $(which zstd) ./zstdcat
./zstdcat hello.zst
