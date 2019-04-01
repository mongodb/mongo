#!/bin/bash
# This script downloads and imports libfmt.

set -vxeuo pipefail

FMT_GIT_URL="https://github.com/mongodb-forks/fmt.git"
FMT_GIT_REV=018d8b57f6ff76fc5bb5abaa243ff3f805bf297f  # 2019-03-30
FMT_GIT_DIR=$(mktemp -d /tmp/import-fmt.XXXXXX)
trap "rm -rf $FMT_GIT_DIR" EXIT

DIST=$(git rev-parse --show-toplevel)/src/third_party/fmt/dist
git clone "$FMT_GIT_URL" $FMT_GIT_DIR
git -C $FMT_GIT_DIR checkout $FMT_GIT_REV

# Exclude the file 'include/format', which provides experimental
# 'std::' definitions.
SUBDIR_WHITELIST=(
    src
    include/fmt
    LICENSE.rst
)
for subdir in ${SUBDIR_WHITELIST[@]}; do
    [[ -d $FMT_GIT_DIR/$subdir ]] && mkdir -p $DIST/$subdir
    cp -Trp $FMT_GIT_DIR/$subdir $DIST/$subdir
done
