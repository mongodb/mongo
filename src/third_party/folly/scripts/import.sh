#!/bin/bash
# This script clones the specified version of folly and strips out everything we do not need.
# Right now, we only use the TokenBucket utility from folly, and so nearly everthing else is removed.

set -o verbose
set -o errexit
set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=folly
VERSION="v2025.04.21.00"
REVISION="${VERSION}-mongo"

# get the source
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/folly
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

git clone --depth 1 --branch $REVISION git@github.com:mongodb-forks/folly.git $DEST_DIR/dist

pushd $DEST_DIR/dist
find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;
find . -mindepth 1 -maxdepth 1 -name "*build*" -exec rm -rf {} \;
find . -mindepth 1 -maxdepth 1 -name "*CMake*" -exec rm -rf {} \;
find . -mindepth 1 -maxdepth 1 -iname "buck*" -exec rm -rf {} \;
rm "CODE_OF_CONDUCT.md"
rm "CONTRIBUTING.md"
rm PACKAGE
rm -rf static
popd

pushd $DEST_DIR/dist/folly
rm -R -- */
find . ! -name 'TokenBucket.h' -type f -exec rm -rf {} +
popd
