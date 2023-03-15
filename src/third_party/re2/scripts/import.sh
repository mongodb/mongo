#!/bin/bash
# This script downloads and imports re2

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=re2
REVISION="2021-09-01"
VERSION="2021-09-01"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/re2
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

git clone --branch 2021-09-01 git@github.com:mongodb-forks/re2.git $DEST_DIR/dist
pushd $DEST_DIR/dist
find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;
rm -rf benchlog
rm -rf doc
rm -rf lib
popd