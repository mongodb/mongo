#!/bin/bash
# This script downloads and patches sqlite
#
# Turn on strict error checking, like perl use 'strict'
set -xeuo pipefail
IFS=$'\n\t'

if [ "$#" -ne 0 ]; then
    echo "This script does not take any arguments"
    exit 1
fi

VERSION=3190300
NAME=sqlite
PNAME=$NAME-amalgamation-$VERSION

SRC_ROOT=$(mktemp -d /tmp/$NAME.XXXXXX)
trap "rm -rf $SRC_ROOT" EXIT
SRC=${SRC_ROOT}/${PNAME}
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$PNAME
PATCH_DIR=$(git rev-parse --show-toplevel)/src/third_party/$PNAME/patches

if [ ! -d $SRC ]; then

    pushd $SRC_ROOT

    wget https://sqlite.org/2017/$PNAME.zip
    unzip $PNAME.zip

    pushd $SRC

    for patch in $PATCH_DIR/*.patch ; do
        patch < $patch
    done

    popd
    popd
fi


test -d $DEST_DIR/$NAME && rm -r $DEST_DIR/$NAME
mkdir -p $DEST_DIR/$NAME

mv $SRC/* $DEST_DIR/$NAME/
