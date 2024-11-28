#!/bin/bash
# This script downloads and imports libstemmer_c.

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=libstemmer_c
VERSION="7b264ffa0f767c579d052fd8142558dc8264d795"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/libstemmer_c/dist
if [[ -d $DEST_DIR ]]; then
    echo "You must remove '$DEST_DIR' before running $0" >&2
    exit 1
fi
mkdir -p $DEST_DIR

SNOWBALL_GIT_DIR=$(mktemp -d /tmp/import-snowball.XXXXXX)
trap "rm -rf $SNOWBALL_GIT_DIR" EXIT

git clone git@github.com:snowballstem/snowball.git $SNOWBALL_GIT_DIR
git -C $SNOWBALL_GIT_DIR checkout $VERSION
pushd $SNOWBALL_GIT_DIR
make dist_libstemmer_c
popd

ARCHIVE=$(find $SNOWBALL_GIT_DIR -regextype posix-extended -regex '^.*\.(tgz|tar\.gz)$')
tar --strip-components=1 -xvzf $ARCHIVE  -C $DEST_DIR
