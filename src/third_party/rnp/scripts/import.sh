#!/bin/bash
# This script downloads and imports RNP - a high performance C++ OpenPGP library.
# RNP is used by Mozilla Thunderbird and provides OpenPGP functionality.
# License: BSD-2-Clause (permissive, suitable for MongoDB)
# Repository: https://github.com/rnpgp/rnp
#
# Dependencies (must be imported separately):
#   - bzip2 (src/third_party/bzip2) - compression library
#   - json-c (src/third_party/json-c) - JSON parsing library

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

set -vxeuo pipefail

FORK_REMOTE=git@github.com:mongodb-forks/rnp.git
VERSION=0.18.1
BRANCH_NAME=v$VERSION

# libsexpp is a git submodule of RNP (S-expressions library)
# License: BSD-2-Clause
# Upstream: https://github.com/rnpgp/sexpp
SEXPP_REMOTE=git@github.com:mongodb-forks/sexpp.git
SEXPP_VERSION=0.9.2
SEXPP_BRANCH=v$SEXPP_VERSION

DEST_DIR="$SCRIPT_DIR/../dist"

# Clean up any existing dist directory
rm -rf "$DEST_DIR"

# Clone RNP
git clone --branch "$BRANCH_NAME" "$FORK_REMOTE" "$DEST_DIR"

# Clone libsexpp submodule manually (fork may not have submodule configured)
rm -rf "$DEST_DIR/src/libsexpp"
git clone --branch "$SEXPP_BRANCH" "$SEXPP_REMOTE" "$DEST_DIR/src/libsexpp"

# Do not commit as a submodule - remove all .git directories
rm -rf "$DEST_DIR/.git"*
rm -rf "$DEST_DIR/src/libsexpp/.git"*

# Generate required headers (config.h, version.h)
"$SCRIPT_DIR/build_rnp_headers.sh"

echo -e "\nrnp imported successfully into $(realpath -s $DEST_DIR)\n"
