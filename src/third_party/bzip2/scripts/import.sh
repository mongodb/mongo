#!/bin/bash
# This script downloads and imports bzip2 - a high-quality data compression library.
# License: BSD-like (bzip2 license)
# Upstream: https://gitlab.com/bzip2/bzip2
# GitHub mirror: https://github.com/libarchive/bzip2

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

set -vxeuo pipefail

FORK_REMOTE=git@github.com:mongodb-forks/bzip2.git
VERSION=1.0.8
BRANCH_NAME=bzip2-$VERSION

DEST_DIR="$SCRIPT_DIR/../dist"

# Clean up any existing dist directory
rm -rf "$DEST_DIR"

git clone --branch "$BRANCH_NAME" "$FORK_REMOTE" "$DEST_DIR"

# Do not commit as a submodule
rm -rf "$DEST_DIR/.git"*

echo -e "\nbzip2 imported successfully into $(realpath -s $DEST_DIR)\n"
