#!/bin/bash
# This script downloads and imports re2

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=re2
REVISION="2025-08-12-mongo"
VERSION="2025-08-12"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/re2
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

mkdir -p $DEST_DIR/dist

git clone --branch $REVISION git@github.com:mongodb-forks/re2.git $DEST_DIR/dist
pushd $DEST_DIR/dist
find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;
rm -rf app benchlog doc lib python
find . -maxdepth 1 -type f -not -regex ".*\(CONTRIBUTING.md\|LICENSE\|README.md\|SECURITY.md\)$" -delete
popd
