#!/bin/sh

set -e

# Test stream size & hint
datagen -g7654 | zstd --stream-size=7654 | zstd -t
datagen -g7654 | zstd --size-hint=7000   | zstd -t
