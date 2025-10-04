#!/bin/sh

set -e

# Test --[no-]compress-literals
zstd file --no-compress-literals -1 -c       | zstd -t
zstd file --no-compress-literals -19 -c      | zstd -t
zstd file --no-compress-literals --fast=1 -c | zstd -t
zstd file --compress-literals -1 -c          | zstd -t
zstd file --compress-literals --fast=1 -c    | zstd -t
