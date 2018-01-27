#!/bin/bash
# This script downloads and imports Google Benchmark.
# It can be run on Linux or Mac OS X.
# Actual integration into the build system is not done by this script.
#
# Turn on strict error checking, like perl use 'strict'
set -xeuo pipefail
IFS=$'\n\t'

if [ "$#" -ne 0 ]; then
    echo "This script does not take any arguments"
    exit 1
fi

VERSION=1.3.0
NAME=benchmark
SRC_ROOT=$(mktemp -d /tmp/benchmark.XXXXXX)
#trap "rm -rf $SRC_ROOT" EXIT
SRC=${SRC_ROOT}/${NAME}-${VERSION}
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME-$VERSION
PATCH_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME-$VERSION/patches

if [ ! -d $SRC ]; then
    git clone git@github.com:google/benchmark.git $SRC

    pushd $SRC
    git checkout v$VERSION
    git am $PATCH_DIR/*.patch
    
    popd
fi

test -d $DEST_DIR/benchmark && rm -r $DEST_DIR/benchmark
mkdir -p $DEST_DIR/benchmark

mv $SRC/.gitignore $DEST_DIR/benchmark/
mv $SRC/include $DEST_DIR/benchmark/
mv $SRC/src $DEST_DIR/benchmark/
mv $SRC/LICENSE $DEST_DIR/benchmark/
mv $SRC/README.md $DEST_DIR/benchmark/
