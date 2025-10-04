#!/bin/sh

set -e

# Test --adapt
zstd -f file --adapt -c | zstd -t

datagen -g100M > file100M

# Pick parameters to force fast adaptation, even on slow systems
zstd --adapt -vvvv -19 --zstd=wlog=10 file100M -o /dev/null 2>&1 | grep -q "faster speed , lighter compression"

# Adaption still happens with --no-progress
zstd --no-progress --adapt -vvvv -19 --zstd=wlog=10 file100M -o /dev/null 2>&1 | grep -q "faster speed , lighter compression"
