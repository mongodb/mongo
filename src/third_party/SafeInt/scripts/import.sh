#!/bin/bash
set -o verbose
set -o errexit

# This script downloads and imports SafeInt.

SAFEINT_GIT_URL="https://raw.githubusercontent.com/mongodb-forks/SafeInt"
# VERSION isn't used to get the SafeInt header here, but marks the release
# version that the file was retrieved from, and silences the sbom_linter.py which
# expects a VERSION.
VERSION="3.0.28a"
REVISION="3.0.28a-mongo"
SAFEINT_GIT_DIR="$(git rev-parse --show-toplevel)/src/third_party/SafeInt"

mkdir -p "${SAFEINT_GIT_DIR}"

wget "${SAFEINT_GIT_URL}/${REVISION}/SafeInt.hpp" \
    -O "${SAFEINT_GIT_DIR}/SafeInt.hpp"

