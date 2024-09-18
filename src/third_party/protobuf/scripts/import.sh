#!/bin/bash
# This script downloads and imports protobuf

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=protobuf
REVISION="v4.25.0"
VERSION="4.25.0"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/protobuf
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

git clone --branch $REVISION https://github.com/mongodb-forks/protobuf.git $DEST_DIR/dist
pushd $DEST_DIR/dist
rm -rf benchmarks
rm -rf cmake
rm -rf conformance
rm -rf docs
rm -rf editors
rm -rf examples
rm -rf kokoro
rm -rf m4
rm -rf protoc-artifacts
rm -rf util
rm -rf ci
rm -rf csharp
rm -rf java
rm -rf lua
rm -rf objectivec
rm -rf php
rm -rf python
rm -rf ruby
rm -rf rust

find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;
find . -type d -name "Google.Protobuf.Test" -exec rm -rf {} \;
popd
