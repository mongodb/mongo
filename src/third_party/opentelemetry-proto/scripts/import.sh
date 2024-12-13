#!/bin/bash
# This script downloads and imports opentelemetry-proto.
#
# Turn on strict error checking, like perl use 'strict'
set -xeuo pipefail
IFS=$'\n\t'

NAME="opentelemetry-proto"

VERSION="mongo/v1.3.2"

LIB_GIT_URL="https://github.com/mongodb-forks/opentelemetry-proto.git"
LIB_GIT_DIR=$(mktemp -d /tmp/import-opentelemetry-proto.XXXXXX)

trap "rm -rf $LIB_GIT_DIR" EXIT

LIBDIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME
DIST=${LIBDIR}/dist
git clone "$LIB_GIT_URL" $LIB_GIT_DIR
git -C $LIB_GIT_DIR checkout $VERSION

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME

SUBDIR_WHITELIST=(
    opentelemetry
    LICENSE
    README.md
)

for subdir in ${SUBDIR_WHITELIST[@]}
do
    [[ -d $LIB_GIT_DIR/$subdir ]] && mkdir -p $DIST/$subdir
    cp -Trp $LIB_GIT_DIR/$subdir $DIST/$subdir
done

for file in $(find -name "*.proto" ); do
    sed -i 's@import "opentelemetry/proto/@import "src/third_party/opentelemetry-proto/opentelemetry/proto/@' $file
done

## Manual steps:
## 1. Apply patch 0001-Add-build-system.patch
## 2. Move code from src/third_party/opentelemetry-proto/dist/ to src/third_party/opentelemetry-proto/
