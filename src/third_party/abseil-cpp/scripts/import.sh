#!/bin/bash

# This script downloads and imports libabsl

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=abseil-cpp
REVISION="20230802.1-mongo-SERVER-88161"
VERSION="20230802.1"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/abseil-cpp
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

git clone --branch $REVISION https://github.com/mongodb-forks/abseil-cpp.git $DEST_DIR/dist
pushd $DEST_DIR/dist
find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;
rm -rf ci
rm -rf scons_gen_build
find absl -depth -type d -name "testdata" -exec rm -rf {} \;
popd
