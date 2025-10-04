#!/bin/sh

# This script copies sources from our fork of asio into "dist/".

# Exit if any command terminates with a nonzero exit status.
set -e

FORK_REMOTE=git@github.com:mongodb-forks/asio.git
# buildscripts/sbom_linter.py checks that the "VERSION=" here matches the "version" property in
# the corresponding "components" element of sbom.json
VERSION=1.34.2
VERSION_SLUG=$(echo "$VERSION" | tr '.' '-')
FORK_BRANCH=asio-$VERSION_SLUG-mongo # e.g. "asio-1-34-2-mongo"
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/asio/dist

# Use git's partial clone and sparse checkout features to pull only the files we want.
git clone --quiet \
          --no-checkout \
          --depth=1 \
          --branch "$FORK_BRANCH" \
          --filter=tree:0 \
          "$FORK_REMOTE" "$DEST_DIR"
cd "$DEST_DIR"
git sparse-checkout init --no-cone
# We want the header files, but not the SSL (TLS) ones, and we want the non-SSL source file.
# Building the non-SSL source file (asio.cpp) is enough to ensure that OpenSSL is not
# initialized by the library at runtime, but to emphasize the point we exclude the SSL code
# entirely.
git sparse-checkout set \
    'asio/include/**/*.ipp' \
    'asio/include/**/*.hpp' \
    '!asio/include/asio/ssl.hpp' \
    '!asio/include/asio/ssl/**/*' \
    'asio/src/asio.cpp'
git checkout --quiet "$FORK_BRANCH"

# Keep the working tree but remove everything else.
rm -rf .git

printf '\nasio imported successfully into %s\n' "$DEST_DIR"
