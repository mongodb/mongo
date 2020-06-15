#!/bin/bash
set -o verbose
set -o errexit

# This script downloads and imports peglib.

PEGLIB_GIT_URL="https://raw.githubusercontent.com/mongodb-forks/cpp-peglib"
PEGLIB_GIT_REV=v0.1.12
PEGLIB_GIT_DIR="$(git rev-parse --show-toplevel)/src/third_party/peglib"

mkdir -p "${PEGLIB_GIT_DIR}"

wget "${PEGLIB_GIT_URL}/${PEGLIB_GIT_REV}/peglib.h" \
    -O "${PEGLIB_GIT_DIR}/peglib.h"

