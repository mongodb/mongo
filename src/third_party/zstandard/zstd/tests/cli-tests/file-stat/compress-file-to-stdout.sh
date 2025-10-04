#!/bin/sh

set -e

datagen > file

zstd file -cq --trace-file-stat > file.zst
zstd -tq file.zst
