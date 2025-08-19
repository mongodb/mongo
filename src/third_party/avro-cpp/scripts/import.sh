#!/bin/bash
# This script downloads and imports Apache Avro C++.

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME="avro-cpp"
VERSION="1.12.0"
BRANCH="branch-1.12"

AVRO_GIT_URL="https://github.com/mongodb-forks/avro.git"

AVRO_GIT_DIR=$(mktemp -d /tmp/import-avro.XXXXXX)
trap "rm -rf $AVRO_GIT_DIR" EXIT

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/avro-cpp/dist

if [[ -d $DEST_DIR ]]; then
    echo "You must remove '$DEST_DIR' before running $0" >&2
    exit 1
fi

git clone --depth 1 --branch $BRANCH "$AVRO_GIT_URL" $AVRO_GIT_DIR

mkdir -p $DEST_DIR

# Copy only the C++ implementation
SELECTED=(
    lang/c++/impl
    lang/c++/include
    lang/c++/AUTHORS
    lang/c++/LICENSE
    lang/c++/NOTICE
    lang/c++/README
    lang/c++/NEWS
    lang/c++/ChangeLog
)

for item in "${SELECTED[@]}"; do
    if [[ -e "$AVRO_GIT_DIR/$item" ]]; then
        cp -r "$AVRO_GIT_DIR/$item" "$DEST_DIR/"
    fi
done

# Rename c++ specific files to root level
if [[ -d "$DEST_DIR/lang/c++" ]]; then
    mv "$DEST_DIR/lang/c++"/* "$DEST_DIR/"
    rm -rf "$DEST_DIR/lang"
fi

# Remove unnecessary files
rm -rf "$DEST_DIR/test" || true
rm -rf "$DEST_DIR/examples" || true
rm -rf "$DEST_DIR/cmake" || true
rm -rf "$DEST_DIR/config" || true
rm -rf "$DEST_DIR/jsonschemas" || true
rm -f "$DEST_DIR/CMakeLists.txt" || true
rm -f "$DEST_DIR/build.sh" || true
rm -f "$DEST_DIR/Doxyfile" || true
rm -f "$DEST_DIR/FindSnappy.cmake" || true
rm -f "$DEST_DIR/MainPage.dox" || true
rm -f "$DEST_DIR/MSBUILD.md" || true
rm -f "$DEST_DIR/.clang-format" || true
rm -f "$DEST_DIR/.gitignore" || true

echo "Avro C++ import completed successfully"
