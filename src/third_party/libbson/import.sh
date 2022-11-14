#!/bin/bash
# This script downloads and imports a revision of libbson.
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

NAME=libbson
REVISION=1.23.0

if grep -q Microsoft /proc/version; then
    SRC_ROOT=$(wslpath -u $(powershell.exe -Command "Get-ChildItem Env:TEMP | Get-Content | Write-Host"))
    SRC_ROOT+="$(mktemp -u /$NAME.XXXXXX)"
    mkdir -p $SRC_ROOT
else
    SRC_ROOT=$(mktemp -d /tmp/$NAME.XXXXXX)
fi
trap "rm -rf $SRC_ROOT" EXIT


SRC=${SRC_ROOT}/mongo-c-driver
CLONE_DEST=$SRC
if grep -q Microsoft /proc/version; then
    CLONE_DEST=$(wslpath -m $SRC)
fi
DEST_DIR=$($GIT_EXE rev-parse --show-toplevel)/src/third_party/libbson/dist

echo "dest: $DEST_DIR"

if [ ! -d $SRC ]; then
    $GIT_EXE clone https://github.com/mongodb/mongo-c-driver $CLONE_DEST

    pushd $SRC
    $GIT_EXE checkout $REVISION
    popd
fi

test -d $DEST_DIR && rm -rf $DEST_DIR
mkdir -p $DEST_DIR

SRC_DIR=${SRC}

mkdir $DEST_DIR/src
cp $SRC_DIR/COPYING $DEST_DIR/
cp $SRC_DIR/THIRD_PARTY_NOTICES $DEST_DIR/
cp -r $SRC_DIR/src/common $DEST_DIR/src
cp -r $SRC_DIR/src/libbson $DEST_DIR/src

stuff_to_remove=(
src/libbson/build
src/libbson/doc
src/libbson/examples
src/libbson/fuzz
src/libbson/tests
)

for file in "${stuff_to_remove[@]}" ; do
    rm -rf "$DEST_DIR/$file"
done
