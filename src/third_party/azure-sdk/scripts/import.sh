#!/bin/bash
# This script downloads and imports the Azure SDK for C++ (specifically azure-storage-blobs)
#
# Following MongoDB's third-party vendoring policy:
# https://github.com/mongodb/mongo/blob/master/src/third_party/README.md

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=azure-sdk
# Azure SDK version - using the azure-storage-blobs package version tag
VERSION="12.13.0"
GIT_TAG="azure-storage-blobs_${VERSION}"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/azure-sdk
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

# Create a temporary directory to clone the SDK
TEMP_DIR=/tmp/azure-sdk-cpp
rm -rf $TEMP_DIR
mkdir -p $TEMP_DIR

# Clone the Azure SDK for C++ repository
git clone --depth 1 --branch $GIT_TAG https://github.com/mongodb-forks/azure-sdk-for-cpp.git $TEMP_DIR

# Setup directories
DIST_DIR=$DEST_DIR/dist
mkdir -p $DIST_DIR

linuxPlatform=linux_x86_64
aarchPlatform=linux_aarch64
PLATFORM_DIR="$DEST_DIR/platform"

# Copy existing platform files if they exist
TEMP_PLATFORM_DIR=/tmp/existing/azure-sdk
rm -rf $TEMP_PLATFORM_DIR
for platform in $linuxPlatform $aarchPlatform
do
    if [[ -d $PLATFORM_DIR/$platform ]]; then
        mkdir -p $TEMP_PLATFORM_DIR/$platform
        cp -r $PLATFORM_DIR/$platform/* $TEMP_PLATFORM_DIR/$platform/ 2>/dev/null || true
    fi
done

# Clean the output directories
rm -rf $DIST_DIR
rm -rf $PLATFORM_DIR

mkdir -p $DIST_DIR

# Copy the necessary SDK components:
# 1. azure-core - Core functionality (HTTP, credentials, etc.)
# 2. azure-identity - Authentication (managed identity, service principal, etc.)
# 3. azure-storage-common - Common storage utilities
# 4. azure-storage-blobs - Blob storage functionality

# Copy azure-core
mkdir -p $DIST_DIR/sdk/core/azure-core
cp -r $TEMP_DIR/sdk/core/azure-core/inc $DIST_DIR/sdk/core/azure-core/
cp -r $TEMP_DIR/sdk/core/azure-core/src $DIST_DIR/sdk/core/azure-core/

# Copy azure-identity (for passwordless authentication)
mkdir -p $DIST_DIR/sdk/identity/azure-identity
cp -r $TEMP_DIR/sdk/identity/azure-identity/inc $DIST_DIR/sdk/identity/azure-identity/
cp -r $TEMP_DIR/sdk/identity/azure-identity/src $DIST_DIR/sdk/identity/azure-identity/

# Copy azure-storage-common
mkdir -p $DIST_DIR/sdk/storage/azure-storage-common
cp -r $TEMP_DIR/sdk/storage/azure-storage-common/inc $DIST_DIR/sdk/storage/azure-storage-common/
cp -r $TEMP_DIR/sdk/storage/azure-storage-common/src $DIST_DIR/sdk/storage/azure-storage-common/

# Copy azure-storage-blobs
mkdir -p $DIST_DIR/sdk/storage/azure-storage-blobs
cp -r $TEMP_DIR/sdk/storage/azure-storage-blobs/inc $DIST_DIR/sdk/storage/azure-storage-blobs/
cp -r $TEMP_DIR/sdk/storage/azure-storage-blobs/src $DIST_DIR/sdk/storage/azure-storage-blobs/

# Copy license files
cp $TEMP_DIR/LICENSE.txt $DIST_DIR/
cp $TEMP_DIR/NOTICE.txt $DIST_DIR/ 2>/dev/null || echo "NOTICE.txt not found, skipping"

# Copy version information
echo "VERSION=$VERSION" > $DIST_DIR/VERSION
echo "GIT_TAG=$GIT_TAG" >> $DIST_DIR/VERSION

# Build and configure for platform-specific headers
HOST_OS="$(uname -s | tr A-Z a-z)"
HOST_ARCH="$(uname -m)"
HOST_DIR="$PLATFORM_DIR/${HOST_OS}_${HOST_ARCH}"

TOOLCHAIN_ROOT=/opt/mongodbtoolchain/v5
if [[ -d $TOOLCHAIN_ROOT ]]; then
    PATH="$TOOLCHAIN_ROOT/bin:$PATH"
    CC=$TOOLCHAIN_ROOT/bin/gcc
    CXX=$TOOLCHAIN_ROOT/bin/g++
else
    CC=${CC:-gcc}
    CXX=${CXX:-g++}
fi

# Build directory for generating platform-specific headers
BUILD_DIR=$TEMP_DIR/build
mkdir -p $BUILD_DIR
pushd $BUILD_DIR

# Configure with CMake to generate headers
cmake $TEMP_DIR \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD=20 \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_C_COMPILER=$CC \
    -DCMAKE_CXX_COMPILER=$CXX \
    -DFETCH_SOURCE_DEPS=OFF \
    -DBUILD_TESTING=OFF \
    -DBUILD_SAMPLES=OFF \
    -DBUILD_DOCUMENTATION=OFF \
    -DBUILD_TRANSPORT_CURL=ON \
    -DBUILD_TRANSPORT_WINHTTP=OFF \
    2>&1 || echo "CMake configuration may have warnings, continuing..."

popd

# Copy generated config headers if they exist
mkdir -p $HOST_DIR/include
if [[ -d $BUILD_DIR/sdk/core/azure-core ]]; then
    find $BUILD_DIR/sdk/core/azure-core -name "*.h" -exec cp {} $HOST_DIR/include/ \; 2>/dev/null || true
fi
if [[ -d $BUILD_DIR/sdk/storage ]]; then
    find $BUILD_DIR/sdk/storage -name "*.h" -exec cp {} $HOST_DIR/include/ \; 2>/dev/null || true
fi

# Restore saved platform files for other architectures
for platform in $linuxPlatform $aarchPlatform
do
    if [[ -d $TEMP_PLATFORM_DIR/$platform ]] && [[ "$platform" != "${HOST_OS}_${HOST_ARCH}" ]]; then
        mkdir -p $PLATFORM_DIR/$platform
        cp -r $TEMP_PLATFORM_DIR/$platform/* $PLATFORM_DIR/$platform/ 2>/dev/null || true
    fi
done

# Cleanup temporary files
rm -rf $TEMP_DIR
rm -rf $TEMP_PLATFORM_DIR

# Remove unnecessary files from dist
pushd $DIST_DIR
find . -type d -name "test" -exec rm -rf {} \; 2>/dev/null || true
find . -type d -name "tests" -exec rm -rf {} \; 2>/dev/null || true
find . -type d -name "samples" -exec rm -rf {} \; 2>/dev/null || true
find . -type d -name ".git" -exec rm -rf {} \; 2>/dev/null || true
find . -type f -name "CMakeLists.txt" -exec rm -f {} \; 2>/dev/null || true
find . -type f -name "*.cmake" -exec rm -f {} \; 2>/dev/null || true
find . -type f -name "*.md" -exec rm -f {} \; 2>/dev/null || true
popd

echo "Azure SDK import complete. Version: $VERSION"

