#!/bin/bash
# This script downloads and imports protobuf

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=protobuf
# Protobuf uses language-specific major versions and minor/patch versions for protoc
# Example: Protobuf C++ version 6.31.1 uses protobuf-cpp v6 and protoc version v31.1
# Vulnerability databases use the full semver for tracking, so specify the full semver version here for the SBOM automation script
# The protobuf repo (and our fork) contain tags for the semver that points to the protoc release
# To determine the correct major version, see: https://protobuf.dev/support/version-support/#cpp
VERSION="v6.31.1"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/protobuf
PATCH_DIR=$(git rev-parse --show-toplevel)/src/third_party/protobuf/patches
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

git clone --branch $VERSION https://github.com/mongodb-forks/protobuf.git $DEST_DIR/dist
pushd $DEST_DIR/dist
#git apply $PATCH_DIR/*.patch
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
rm -rf compatibility

find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;
find . -type d -name "Google.Protobuf.Test" -exec rm -rf {} \;
popd
