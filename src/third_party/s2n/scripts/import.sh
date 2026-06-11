#!/bin/bash
# This script downloads only the necessary source code files for s2n-tls from
# the requested version branch on mongodb-forks/s2n-tls.
# Note that a corresponding branch in the format "s2n-<majorVersion>-<minorVersion>-<patchVersion>-mongo"
# must already exist on mongodb-forks/s2n-tls based on the corresponding
# version tag from the upstream aws/s2n-tls repo.

set -e
set -x

REMOTE_REPO=git@github.com:mongodb-forks/s2n-tls.git
VERSION=1.7.3
VERSION_BRANCH=s2n-$(echo "$VERSION" | tr '.' '-')-mongo # e.g. "s2n-1-7-3-mongo"
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/s2n/dist

# Use git's partial clone and sparse checkout features to pull only the files we want.
git clone --quiet \
          --no-checkout \
          --depth=1 \
          --branch "$VERSION_BRANCH" \
          --filter=tree:0 \
          "$REMOTE_REPO" "$DEST_DIR"
cd "$DEST_DIR"
git sparse-checkout init --no-cone

# We only need to copy over the 6 directories that contain all of the source code.
# The remaining directories and files are for tests, licensing, and various build/release
# infra.
git sparse-checkout set --no-cone \
    'api/' \
    'crypto/' \
    'error/' \
    'stuffer/' \
    'tls/' \
    'utils/' \
    '/LICENSE' \
    '/NOTICE'
git checkout --quiet "$VERSION_BRANCH"

# Keep the working tree but remove git-related information.
rm -rf .git
