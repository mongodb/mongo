#!/bin/bash
# This script downloads and imports immer.

set -vxeuo pipefail

IMMER_GIT_URL="https://github.com/mongodb-forks/immer.git"

IMMER_GIT_REV=d98a68cd6c60e025547614ad0809f68fd816d740

LIB_GIT_DIR=$(mktemp -d /tmp/import-immer.XXXXXX)
trap "rm -rf $LIB_GIT_DIR" EXIT

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/immer
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

git clone "$IMMER_GIT_URL" $LIB_GIT_DIR
git -C $LIB_GIT_DIR checkout $IMMER_GIT_REV

# Remove unnecessary stuff
rm -rf $LIB_GIT_DIR/benchmark
rm -rf $LIB_GIT_DIR/cmake
rm -rf $LIB_GIT_DIR/doc
rm -rf $LIB_GIT_DIR/example
rm -rf $LIB_GIT_DIR/extra
rm -rf $LIB_GIT_DIR/nix
rm -rf $LIB_GIT_DIR/test
rm -rf $LIB_GIT_DIR/tools/clojure
rm -rf $LIB_GIT_DIR/tools/docker
rm -rf $LIB_GIT_DIR/tools/include
rm -rf $LIB_GIT_DIR/tools/scala
rm -rf $LIB_GIT_DIR/tools/sinusoidal-sphinx-theme
rm -rf $LIB_GIT_DIR/tools/reproduce-paper-results.bash
rm -rf $LIB_GIT_DIR/tools/with-tee.bash
rm -f $LIB_GIT_DIR/BUILD
rm -f $LIB_GIT_DIR/CMakeLists.txt
rm -f $LIB_GIT_DIR/Package.swift
rm -f $LIB_GIT_DIR/WORKSPACE
rm -f $LIB_GIT_DIR/codecov.yml
rm -f $LIB_GIT_DIR/default.nix
rm -f $LIB_GIT_DIR/setup.py
rm -f $LIB_GIT_DIR/shell.nix
rm -f $LIB_GIT_DIR/spm.cpp

test -d $DEST_DIR/dist && rm -r $DEST_DIR/dist
mkdir -p $DEST_DIR/dist
mv $LIB_GIT_DIR/* $DEST_DIR/dist
