#!/bin/bash
# This script downloads and imports cpptrace

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=cpptrace
VERSION="1.0.3"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/cpptrace
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

mkdir -p $DEST_DIR/dist

wget -P $DEST_DIR/dist https://github.com/jeremy-rifkin/cpptrace/archive/refs/tags/v$VERSION.tar.gz
trap "rm -rf $DEST_DIR/dist/v$VERSION.tar.gz" EXIT
tar --strip-components=1 -xvzf $DEST_DIR/dist/v$VERSION.tar.gz -C $DEST_DIR/dist

pushd $DEST_DIR/dist
find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;
rm -rf benchmarking ci cmake docs res test tools
find . -maxdepth 1 -type f -not -regex ".*\(CHANGELOG.md\|CONTRIBUTING.md\|LICENSE\|README.md\|SECURITY.md\)$" -delete
popd
