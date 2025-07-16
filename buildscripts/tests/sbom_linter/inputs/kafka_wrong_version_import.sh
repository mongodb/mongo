#!/bin/bash

# This script creates a copy of sources for librdkafka and librdkafka++.
# This script currently only works on Linux x86_64 and aarch64 platforms.

set -euo pipefail
IFS=$'\n\t'

if [ "$#" -ne 0 ]; then
    echo "This script does not take any arguments"
    exit 1
fi

# Create a temporary directory to clone and configure librdkafka
TEMP_DIR=$(mktemp -d /tmp/librdkafka.XXXXXX)

# Setup some directory variables
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/librdkafka
DIST_DIR=$DEST_DIR/dist
PLATFORM_DIR=$DIST_DIR/platform
VERSION = "wrong version"

# Clean the output directories
rm -rf $DIST_DIR
rm -rf $PLATFORM_DIR
rm -rf $TEMP_DIR/*

pushd $TEMP_DIR

# Clone the v2.0.2 branch of librdkafka.
git clone --depth 1 --branch v2.0.2 https://github.com/confluentinc/librdkafka.git

pushd librdkafka

echo "Generating config.h"

# Run configure to generate config.h, and move it into a platform specific directory.
./configure --source-deps-only
platformName=linux_$(uname -m)
# Copy the config.h into a platform specific directory
mkdir -p $PLATFORM_DIR/$platformName/include
mv config.h $PLATFORM_DIR/$platformName/include

# Remove un-used files
rm -rf CHANGELOG.md CODE_OF_CONDUCT.md CONFIGURATION.md CONTRIBUTING.md INTRODUCTION.md \
    README.md README.win32 STATISTICS.md config.log.old dev-conf.sh examples/ \
    CMakeLists.txt lds-gen.py mklove/ packaging/ service.yml tests/ vcpkg.json win32/ \
    Makefile Makefile.config config.cache configure.self configure debian mainpage.doxy Doxyfile \
    src/CMakeLists.txt src/Makefile src/generate_proto.sh src/librdkafka_cgrp_synch.png src/statistics_schema.json \
    src-cpp/CMakeLists.txt src-cpp/Makefile src-cpp/README.md config.log

pushd src
# Replace all instances of the string "LZ4" and "XXH" with "KLZ4" and "KXXH" in the C source code.
# This is to avoid symbol conflicts with the LZ4 and XXH source that is used by
# third_party/mozjs.
sed -i 's/LZ4/KLZ4/g' *
sed -i 's/XXH/KXXH/g' *
popd

mkdir -p $DIST_DIR
cp -r * $DIST_DIR

popd
popd
