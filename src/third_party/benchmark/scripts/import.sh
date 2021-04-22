#!/bin/bash
# This script downloads and imports Google Benchmark.
#
# Turn on strict error checking, like perl use 'strict'
set -xeuo pipefail
IFS=$'\n\t'

NAME="benchmark"

LIB_GIT_REV="mongo/v1.5.2"

LIB_GIT_URL="https://github.com/mongodb-forks/benchmark.git"
LIB_GIT_DIR=$(mktemp -d /tmp/import-benchmark.XXXXXX)

trap "rm -rf $LIB_GIT_DIR" EXIT

DIST=$(git rev-parse --show-toplevel)/src/third_party/$NAME/dist
git clone "$LIB_GIT_URL" $LIB_GIT_DIR
git -C $LIB_GIT_DIR checkout $LIB_GIT_REV

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME

SUBDIR_WHITELIST=(
    src
    include/benchmark
    LICENSE
    README.md
)

for subdir in ${SUBDIR_WHITELIST[@]}
do
    [[ -d $LIB_GIT_DIR/$subdir ]] && mkdir -p $DIST/$subdir
    cp -Trp $LIB_GIT_DIR/$subdir $DIST/$subdir
done
