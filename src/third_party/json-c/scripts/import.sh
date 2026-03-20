#!/bin/bash
# This script downloads and imports json-c - a JSON implementation in C.
# License: MIT
# Repository: https://github.com/json-c/json-c

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

set -vxeuo pipefail

FORK_REMOTE=git@github.com:mongodb-forks/json-c.git
VERSION=0.17
BRANCH_NAME=json-c-$VERSION-20230812

DEST_DIR="$SCRIPT_DIR/../dist"

# Clean up any existing dist directory
rm -rf "$DEST_DIR"

git clone --branch "$BRANCH_NAME" "$FORK_REMOTE" "$DEST_DIR"

# Do not commit as a submodule
rm -rf "$DEST_DIR/.git"*

# Remove tests directory (not needed, avoids format check issues)
rm -rf "$DEST_DIR/tests"

# Generate required headers (config.h, json_config.h)
"$SCRIPT_DIR/build_json_c_headers.sh"

echo -e "\njson-c imported successfully into $(realpath -s $DEST_DIR)\n"
