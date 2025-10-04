#!/bin/bash
# This script downloads and imports nlohmann-json.
#
# Turn on strict error checking, like perl use 'strict'
set -xeuo pipefail
IFS=$'\n\t'

NAME="nlohmann-json"

VERSION="mongo/v3.11.3"

LIB_GIT_URL="https://github.com/mongodb-forks/json.git"
LIB_GIT_DIR=$(mktemp -d /tmp/import-nlohmann-json.XXXXXX)

trap "rm -rf $LIB_GIT_DIR" EXIT

LIBDIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME
DIST=${LIBDIR}/
git clone "$LIB_GIT_URL" $LIB_GIT_DIR
git -C $LIB_GIT_DIR checkout $VERSION

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME

SUBDIR_WHITELIST=(
    include
    LICENSE.MIT
    README.md
)

for subdir in ${SUBDIR_WHITELIST[@]}
do
    [[ -d $LIB_GIT_DIR/$subdir ]] && mkdir -p $DIST/$subdir
    cp -Trp $LIB_GIT_DIR/$subdir $DIST/$subdir
done
