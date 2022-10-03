#!/bin/bash
# This script downloads and imports cares

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=cares
REVISION="cares-1_17_2"
VERSION="1.17.2"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/cares
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

git clone --branch cares-1_17_2 git@github.com:mongodb-forks/c-ares.git $DEST_DIR/dist

pushd $DEST_DIR/dist
autoreconf -i
./configure
make dist

find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;
rm -rf *.cache
rm -rf travis
rm -rf *.tar.gz

popd


