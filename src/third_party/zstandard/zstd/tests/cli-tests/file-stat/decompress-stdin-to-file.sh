#!/bin/sh

set -e

datagen | zstd -q > file.zst

zstd -dcq --trace-file-stat < file.zst -o file
