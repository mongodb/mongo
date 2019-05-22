#!/bin/bash
set -o verbose
set -o errexit

# This script downloads and imports SafeInt.

SAFEINT_GIT_URL="https://github.com/mongodb-forks/SafeInt"
SAFEINT_GIT_REV=b81a4a5d710f3166a04d77a3600035cf185e6c09  # Version 3.0.20p, 2018-12-11
SAFEINT_GIT_DIR=$(mktemp -d /tmp/import-safeint.XXXXXX)
trap "rm -rf $SAFEINT_GIT_DIR" EXIT

DIST=src/third_party/SafeInt
git clone "$SAFEINT_GIT_URL" $SAFEINT_GIT_DIR
git -C $SAFEINT_GIT_DIR checkout $SAFEINT_GIT_REV

cp -rp $SAFEINT_GIT_DIR/SafeInt.hpp $DIST/
