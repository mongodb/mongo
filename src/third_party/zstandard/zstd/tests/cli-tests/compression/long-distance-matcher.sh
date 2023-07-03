#!/bin/sh

set -e

# Test --long
zstd -f file --long   ; zstd -t file.zst
zstd -f file --long=20; zstd -t file.zst
