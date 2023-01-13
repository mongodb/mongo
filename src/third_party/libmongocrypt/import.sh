#!/bin/bash
# This script downloads and imports a revision of libmongocrypt.
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

NAME=libmongocrypt
REVISION=2c564d555b168e9bdb60ade186b6f3aa759e010b

if grep -q Microsoft /proc/version; then
    SRC_ROOT=$(wslpath -u $(powershell.exe -Command "Get-ChildItem Env:TEMP | Get-Content | Write-Host"))
    SRC_ROOT+="$(mktemp -u /$NAME.XXXXXX)"
    mkdir -p $SRC_ROOT
else
    SRC_ROOT=$(mktemp -d /tmp/$NAME.XXXXXX)
fi
trap "rm -rf $SRC_ROOT" EXIT


SRC=${SRC_ROOT}/${NAME}
CLONE_DEST=$SRC
if grep -q Microsoft /proc/version; then
    CLONE_DEST=$(wslpath -m $SRC)
fi
DEST_DIR=$($GIT_EXE rev-parse --show-toplevel)/src/third_party/libmongocrypt/dist

echo "dest: $DEST_DIR"

if [ ! -d $SRC ]; then
    $GIT_EXE clone https://github.com/mongodb/libmongocrypt $CLONE_DEST

    pushd $SRC
    $GIT_EXE checkout $REVISION
    popd
fi

test -d $DEST_DIR && rm -rf $DEST_DIR
mkdir -p $DEST_DIR

SRC_DIR=${SRC}

cp -r $SRC_DIR/src $DEST_DIR/src
cp -r $SRC_DIR/kms-message $DEST_DIR/kms-message

stuff_to_remove=(
bindings
cmake
debian
doc
etc
kms-message/aws-sig-v4-test-suite
kms-message/CMakeLists.txt
kms-message/cmake
kms-message/README.rst
kms-message/test
test
)

for file in "${stuff_to_remove[@]}" ; do
    rm -rf "$DEST_DIR/$file"
done

