#!/bin/bash
# Downloads and imports tcmalloc

set -euo pipefail
IFS=$'\n\t'

set -vx

VERSION=20230227-snapshot-093ba93c
LIB_GIT_REVISION=mongo-20240522
LIB_GIT_REPO=git@github.com:mongodb-forks/tcmalloc.git

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/tcmalloc
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

LIB_GIT_DIR=$(mktemp -d /tmp/import-tcmalloc.XXXXXX)
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
find tcmalloc -type d -name "testdata" -exec rm -rf {} \;
popd
