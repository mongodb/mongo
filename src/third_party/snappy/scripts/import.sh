#!/bin/bash
# This script clones the specified version of snappy

set -o verbose
set -o errexit
set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=snappy
REVISION="v1.1.10-SERVER-85283"
VERSION="1.1.10"

# get the source
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/snappy
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

git clone --depth 1 --branch $REVISION git@github.com:mongodb-forks/snappy.git $DEST_DIR/dist

pushd $DEST_DIR/dist
find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;
find . -mindepth 1 -maxdepth 1 -name "*test*" -exec rm -rf {} \;
find . -mindepth 1 -maxdepth 1 -name "*benchmark*" -exec rm -rf {} \;
find . -mindepth 1 -maxdepth 1 -name "*fuzzer*" -exec rm -rf {} \;
rm -rf .github/workflows
rm -rf docs
rm -rf testdata
rm -rf third_party
popd