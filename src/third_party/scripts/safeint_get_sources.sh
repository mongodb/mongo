#!/bin/bash
set -o verbose
set -o errexit

# This script downloads and imports SafeInt.

SAFEINT_GIT_URL="https://raw.githubusercontent.com/mongodb-forks/SafeInt"
SAFEINT_GIT_REV=3.0.26
SAFEINT_GIT_DIR="$(git rev-parse --show-toplevel)/src/third_party/SafeInt"

mkdir -p "${SAFEINT_GIT_DIR}"

wget "${SAFEINT_GIT_URL}/${SAFEINT_GIT_REV}/SafeInt.hpp" \
    -O "${SAFEINT_GIT_DIR}/SafeInt.hpp"

