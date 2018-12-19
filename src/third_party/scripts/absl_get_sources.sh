#!/bin/bash
# This script downloads and imports a revision of abseil-cpp.
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

NAME=abseil-cpp
REVISION=111ca7060a6ff50115ca85b59f6b5d8c8c5e9105
if grep -q Microsoft /proc/version; then
    SRC_ROOT=$(wslpath -u $(powershell.exe -Command "Get-ChildItem Env:TEMP | Get-Content | Write-Host"))
    SRC_ROOT+="$(mktemp -u /abseil-cpp.XXXXXX)"
    mkdir -p $SRC_ROOT
else
    SRC_ROOT=$(mktemp -d /tmp/abseil-cpp.XXXXXX)
fi

SRC=${SRC_ROOT}/${NAME}_${REVISION}
CLONE_DEST=$SRC
if grep -q Microsoft /proc/version; then
    CLONE_DEST=$(wslpath -m $SRC)
fi
DEST_DIR=$($GIT_EXE rev-parse --show-toplevel)/src/third_party/$NAME-master
PATCH_DIR=$($GIT_EXE rev-parse --show-toplevel)/src/third_party/$NAME-master/patches
if grep -q Microsoft /proc/version; then
    DEST_DIR=$(wslpath -u "$DEST_DIR")
    PATCH_DIR=$(wslpath -w $(wslpath -u "$PATCH_DIR"))
fi

echo "dest: $DEST_DIR"
echo "patch: $PATCH_DIR"

if [ ! -d $SRC ]; then
    $GIT_EXE clone git@github.com:abseil/abseil-cpp.git $CLONE_DEST

    pushd $SRC
    $GIT_EXE checkout $REVISION
    for p in $(shopt -s nullglob; echo $PATCH_DIR/*.patch); do
        $GIT_EXE am "$p"
    done
    
    popd
fi

test -d $DEST_DIR/abseil-cpp && rm -r $DEST_DIR/abseil-cpp
mkdir -p $DEST_DIR/abseil-cpp
mv $SRC/* $DEST_DIR/abseil-cpp
