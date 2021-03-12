#!/bin/bash
# This script downloads and imports libfmt.

set -vxeuo pipefail

FMT_GIT_URL="https://github.com/mongodb-forks/fmt.git"

FMT_GIT_REV=7.1.3

FMT_GIT_DIR=$(mktemp -d /tmp/import-fmt.XXXXXX)
trap "rm -rf $FMT_GIT_DIR" EXIT

DIST=$(git rev-parse --show-toplevel)/src/third_party/fmt/dist
git clone "$FMT_GIT_URL" $FMT_GIT_DIR
git -C $FMT_GIT_DIR checkout $FMT_GIT_REV

rm -rf "$DIST"
mkdir -p "$DIST"

# Exclude the file 'include/format', which provides experimental
# 'std::' definitions.
SELECTED=(
    src
    include/fmt
    LICENSE.rst
)

tar -C "$FMT_GIT_DIR" -c -f - "${SELECTED[@]}" | tar -C "$DIST" -x -f -
