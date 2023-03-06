#!/bin/bash
# This script downloads and imports libunwind

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=abseil-cpp

LIB_GIT_REVISION=20210324.1
LIB_GIT_REPO=git@github.com:mongodb-forks/abseil-cpp.git

# misnamed, we aren't actually taking their 'master' branch.
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/abseil-cpp
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

LIB_GIT_DIR=$(mktemp -d /tmp/import-abseil-cpp.XXXXXX)
trap "rm -rf $LIB_GIT_DIR" EXIT

git clone $LIB_GIT_REPO $LIB_GIT_DIR
git -C $LIB_GIT_DIR checkout $LIB_GIT_REVISION

test -d $DEST_DIR/dist && rm -r $DEST_DIR/dist
mkdir -p $DEST_DIR/dist
mv $LIB_GIT_DIR/* $DEST_DIR/dist

pushd $DEST_DIR/dist
find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;
rm -rf ci
rm -rf scons_gen_build
find absl -type d -name "testdata" -exec rm -rf {} \;
popd
