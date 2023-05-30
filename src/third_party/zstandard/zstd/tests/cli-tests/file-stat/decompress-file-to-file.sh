#!/bin/sh

set -e

datagen | zstd -q > file.zst
chmod 642 file.zst

zstd -dq --trace-file-stat file.zst
