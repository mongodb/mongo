#!/bin/bash

# This script downloads and imports xxHash.

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=xxhash
REVISION="v0.8.3-mongo"
VERSION="v0.8.3"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

git clone --depth 1 --branch $REVISION git@github.com:mongodb-forks/xxHash.git $DEST_DIR/dist

pushd $DEST_DIR/dist
 # Remove dotfiles, CI config, tests, fuzzers, the build system, and the xxhsum
 # CLI. Keep the library sources, public headers, and top-level metadata files (LICENSE/README/SECURITY/CHANGELOG).
find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;
rm -rf cli cmake_unofficial doc fuzz tests
rm -f Doxyfile Doxyfile-internal Makefile appveyor.yml clib.json libxxhash.pc.in xxhsum.c
popd
