#!/bin/bash
# This script downloads and imports a revision of kms-message.
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

NAME=kms-message
REVISION=8d91fa28cf179be591f595ca6611f74443357fdb

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
DEST_DIR=$($GIT_EXE rev-parse --show-toplevel)/src/third_party/$NAME

echo "dest: $DEST_DIR"

if [ ! -d $SRC ]; then
    $GIT_EXE clone https://github.com/mongodb-labs/kms-message $CLONE_DEST

    pushd $SRC
    $GIT_EXE checkout $REVISION
    popd
fi

test -d $DEST_DIR/$NAME && rm -r $DEST_DIR/$NAME
mkdir -p $DEST_DIR/$NAME

stuff_to_remove=(
aws-sig-v4-test-suite
CMakeLists.txt
cmake
README.rst
test
)

for file in "${stuff_to_remove[@]}" ; do
    rm -rf "$SRC/$file"
done

cp -r $SRC/* $DEST_DIR


