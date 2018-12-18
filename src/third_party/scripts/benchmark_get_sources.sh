#!/bin/bash
# This script downloads and imports Google Benchmark.
# It can be run on Linux, Mac OS X or Windows WSL.
# Actual integration into the build system is not done by this script.
#
# Turn on strict error checking, like perl use 'strict'
set -xeuo pipefail
IFS=$'\n\t'

if [ "$#" -ne 0 ]; then
    echo "This script does not take any arguments"
    exit 1
fi

GIT_EXE=git
if grep -q Microsoft /proc/version; then
    GIT_EXE=git.exe
fi

NAME=benchmark
VERSION=1.4.1
if grep -q Microsoft /proc/version; then
    SRC_ROOT=$(wslpath -u $(powershell.exe -Command "Get-ChildItem Env:TEMP | Get-Content | Write-Host"))
    SRC_ROOT+="$(mktemp -u /benchmark.XXXXXX)"
    mkdir -p $SRC_ROOT
else
    SRC_ROOT=$(mktemp -d /tmp/benchmark.XXXXXX)
fi

SRC=${SRC_ROOT}/${NAME}-${VERSION}
CLONE_DEST=$SRC
if grep -q Microsoft /proc/version; then
    CLONE_DEST=$(wslpath -m $SRC)
fi
DEST_DIR=$($GIT_EXE rev-parse --show-toplevel)/src/third_party/$NAME-$VERSION
PATCH_DIR=$($GIT_EXE rev-parse --show-toplevel)/src/third_party/$NAME-$VERSION/patches
if grep -q Microsoft /proc/version; then
    DEST_DIR=$(wslpath -u "$DEST_DIR")
    PATCH_DIR=$(wslpath -w $(wslpath -u "$PATCH_DIR"))
fi

echo "dest: $DEST_DIR"
echo "patch: $PATCH_DIR"

if [ ! -d $SRC ]; then
    $GIT_EXE clone git@github.com:google/benchmark.git $CLONE_DEST

    pushd $SRC
    $GIT_EXE checkout v$VERSION

    $GIT_EXE am $PATCH_DIR/0001-properly-escape-json-names-652.patch
    
    popd
fi

test -d $DEST_DIR/benchmark && rm -r $DEST_DIR/benchmark
mkdir -p $DEST_DIR/benchmark

mv $SRC/.gitignore $DEST_DIR/benchmark/
mv $SRC/include $DEST_DIR/benchmark/
mv $SRC/src $DEST_DIR/benchmark/
mv $SRC/LICENSE $DEST_DIR/benchmark/
mv $SRC/README.md $DEST_DIR/benchmark/
