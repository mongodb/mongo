#!/bin/bash
# This script downloads and imports libunwind

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=libunwind
REVISION="v1.5-stable-mongo" # 2021-01-13
VERSION="1.5.0"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/unwind
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

UNWIND_GIT_DIR=$(mktemp -d /tmp/import-libunwind.XXXXXX)
trap "rm -rf $UNWIND_GIT_DIR" EXIT

git clone git@github.com:mongodb-forks/libunwind.git $UNWIND_GIT_DIR
git -C $UNWIND_GIT_DIR checkout $REVISION

pushd $UNWIND_GIT_DIR
NOCONFIGURE=1 ./autogen.sh
./configure
make dist
popd

DIST_TGZ=$UNWIND_GIT_DIR/libunwind-$VERSION.tar.gz

mkdir -p $DEST_DIR/dist
tar --strip-components=1 -xvzf $DIST_TGZ  -C $DEST_DIR/dist
# mv libunwind-$VERSION libunwind
