#!/bin/bash
# This script downloads and imports googletest (including googlemock).

set -vxeuo pipefail

GOOGLETEST_GIT_URL="https://github.com/mongodb-forks/googletest.git"

VERSION=1.17.0
BRANCH=v${VERSION}-mongodb

GOOGLETEST_GIT_DIR=$(mktemp -d /tmp/import-googletest.XXXXXX)
trap "rm -rf $GOOGLETEST_GIT_DIR" EXIT

DIST=$(git rev-parse --show-toplevel)/src/third_party/googletest_restricted_for_disagg_only/dist
git clone "$GOOGLETEST_GIT_URL" $GOOGLETEST_GIT_DIR
git -C $GOOGLETEST_GIT_DIR checkout $BRANCH

rm -rf "$DIST"
mkdir -p "$DIST"

# Skipping WORKSPACE AND MODULE.bazel because we want to avoid the abseil and re2 dependencies.
SELECTED=(
    LICENSE
    README.md
    googlemock/include
    googlemock/src
    googletest/include
    googletest/src
)

tar -C "$GOOGLETEST_GIT_DIR" -c -f - "${SELECTED[@]}" | tar -C "$DIST" -x -f -
