#!/bin/bash

set -euo pipefail

if [ "$#" -ne 0 ]; then
    echo "This script does not take any arguments"
    exit 1
fi

GTEST_GIT_URL="https://github.com/mongodb-forks/googletest"
VERSION=1.17.0
GTEST_GIT_BRANCH=v${VERSION}-mongo

GIT_ROOT=$(git rev-parse --show-toplevel)
DEST_DIR=$GIT_ROOT/src/third_party/googletest
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

# Import files from our fork of googletest.
import() {
    LIB_GIT_DIR=$(mktemp -d /tmp/import-googletest.XXXXXX)
    pushd $LIB_GIT_DIR
    trap "rm -rf $LIB_GIT_DIR" EXIT RETURN

    git clone "$GTEST_GIT_URL" -b $GTEST_GIT_BRANCH $LIB_GIT_DIR

    test -d $DEST_DIR/dist && rm -r $DEST_DIR/dist
    mkdir -p $DEST_DIR/dist
    mv $LIB_GIT_DIR/* $DEST_DIR/dist

    popd
}
import

# Trim files.
trim() {
    pushd $DEST_DIR

    # Strip directories that exclusively contain non-source.
    STRIP_DIRS=(
        ci
        docs
        test
        samples
        cmake
    )
    for STRIP_DIR in ${STRIP_DIRS[@]}; do
        find dist -type d -name $STRIP_DIR -print0 | xargs -0 rm -rf
    done

    # Strip files of extensions that are non-source.
    STRIP_EXTS=(
        txt
        md
        bazel
        bzl
        bzlmod
    )
    for STRIP_EXT in ${STRIP_EXTS[@]}; do
        find dist -name "*.$STRIP_EXT" -print0 | xargs -0 rm -f
    done

    # Strip files that exclusively contain non-source.
    STRIP_FILES=(
        WORKSPACE
    )
    for STRIP_FILE in ${STRIP_FILES[@]}; do
        find dist -type f -name $STRIP_FILE -print0 | xargs -0 rm -rf
    done

    popd
}
trim
