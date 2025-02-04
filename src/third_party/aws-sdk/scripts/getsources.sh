#!/bin/bash
set -e
set -x

# This script creates a copy of sources for aws-sdk-cpp.
# This script currently only works on Linux x86_64 platforms.

if [ "$#" -ne 0 ]; then
	echo "This script does not take any arguments"
	exit 1
fi

# Create a temporary directory to clone and configure
TEMP_DIR=/tmp/aws-sdk-cpp
rm -rf $TEMP_DIR
mkdir $TEMP_DIR

# Setup the directory names
SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
BASE_DIR="$SCRIPT_DIR/.."
mkdir -p $BASE_DIR
DIST_DIR=$BASE_DIR/dist
linuxPlatform=linux_x86_64
aarchPlatform=linux_aarch64
PLATFORM_DIR="$BASE_DIR/platform"

# Copy existing platform files. For future version upgrades, we may have to run the cmake invocation
# to configure the aws-sdk-cpp repo instead of reusing existing platform files.
TEMP_PLATFORM_DIR=/tmp/existing/aws-sdk
rm -rf $TEMP_PLATFORM_DIR
for platform in $linuxPlatform $aarchPlatform
do
  mkdir -p $TEMP_PLATFORM_DIR/$platform
  cp -r $PLATFORM_DIR/$platform/aws-sdk $TEMP_PLATFORM_DIR/$platform
done

# Clean the output directories
rm -rf $DIST_DIR
rm -rf $PLATFORM_DIR

cd $TEMP_DIR
# Get the release of aws-sdk-cpp
VERSION=1.11.471
git clone --recurse-submodules --depth 1 --branch $VERSION https://github.com/aws/aws-sdk-cpp.git .

mkdir build
cd build

# Configure and create the final Makefile
# If making platform files use "cmake .." instead of "cmake ."
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_STANDARD=17 -DBUILD_ONLY="s3;lambda;kinesis" -DENABLE_UNITY_BUILD=OFF -DENABLE_TESTING=OFF -DAUTORUN_UNIT_TESTS=OFF -DENABLE_HTTP_CLIENT_TESTING=OFF -DCMAKE_INSTALL_PREFIX=..
cmake --build . --config=Debug
cmake --install . --config=Debug

cd $TEMP_DIR

# Uncomment to copy platform files to platform folders
# find . -type f -iname "*.h" -exec cp --parents {} $TEMP_PLATFORM_DIR/$linuxPlatform \;

# Copy the source files
mkdir -p $DIST_DIR/aws-sdk

find . -type f \( -iname "*.h" -o -iname "*.c" -o -iname "*.cpp" -o -iname "*.def" -o -iname "*.inl" -o -iname "*license*" -o -iname "*clang*" -o -iname "*notice*" \) -exec cp --parents {} $DIST_DIR/aws-sdk/ ';'

# Copy over everything
# cp -r . $DIST_DIR/aws-sdk

# Copy the platform specified files we had saved earlier
for platform in $linuxPlatform $aarchPlatform
do
  mkdir -p $PLATFORM_DIR/$platform/aws-sdk
  cp -r $TEMP_PLATFORM_DIR/$platform/aws-sdk $PLATFORM_DIR/$platform
done

# Cleanup files that are not used
cd $DIST_DIR/aws-sdk

find . -type d \( -name "build" -o -name "aws-lc" -o -name "bin" -o -name "tests" -o -name "verification" -o -name "integration-testing" -o -name "smoke-tests" -o -name "tools" -o -name "AWSCRTAndroidTestRunner" -o -name "testing" -o -name "samples" -o -name "docs" -o -name "CMakeFiles" -o -name ".github" -o -name "clang-tidy" -o -name "codebuild" -o -name ".builder"  -o -name "toolchains" \) -exec rm -rf {} \; || true

# sub folders in source folders that are not needed
find . -type d \( -name "darwin" -o -name "windows" -o -name "android" -o -name "msvc" -o -name "msvc" -o -name "platform_fallback_stubs" -o -name "huffman_generator" -o -name "bsd" \) -exec rm -rf {} \; || true

cd $DIST_DIR/aws-sdk/src
find . -maxdepth 1 -type d -not \( -name "aws-cpp-sdk-core" -o -name "aws-cpp-sdk-access-management" \) -exec rm -rf {} \; || true
cd $DIST_DIR/aws-sdk/generated/src
find . -maxdepth 1 -type d -not \( -name "aws-cpp-sdk-kinesis" -o -name "aws-cpp-sdk-lambda" -o -name "aws-cpp-sdk-s3" -o -name "aws-cpp-sdk-iam" -o -name "aws-cpp-sdk-cognito-identity" \) -exec rm -rf {} \; || true

# Cleanup for copying over everything
# find . -type f,l \( -iname ".gitignore" -o -iname ".git" -o -iname ".gitattributes" -o -iname ".gitmodules"  -o -iname ".git-blame-ignore-revs" -o -iname "*.o" -o -iname "*.so" -o -iname "*.so.*" -o -iname "*.o.d" -o -iname "*.i" -o -iname "*.s" -o -iname "*.out" -o -iname "*.md" -o -iname "Makefile" -o -iname "*.sh" \) -exec rm {} \;
# find . -type f,l \( -iname "*.cmake" -o -iname "CMakeCache.txt" -o -iname "prefetch_crt_dependency.sh" -o -iname "builder.json" -o -iname "CMakeLists.txt" -o -iname "*.py" -o -iname "*.pep8" -o -iname "flake.lock" -o -iname "flake.nix" -o -iname "*.mk" -o -iname "versioning.rst" \) -exec rm {} \;
# # the directory cleanups sometimes need to be ran manually
# find . -type d \( -name ".git" -o -name "googletest" \) -exec rm -rf {} \;
# find . -type d \( -name "cmake" -o -name "doxygen" -o -name "AWSSDK" -o -name "cmake" -o -name "aws-lc" \) -exec rm -rf {} \;
# find . -type d \( -name "ecdsa-fuzz-corpus" -o -name "scripts" -o -name ".travis" -o -name "docsrc" -o -name "coverage" -o -name "bindings" -o -name "compliance" -o -name "lib" -o -name "libcrypto-build" -o -name "nix" -o -name "scram" \) -exec rm -rf {} \;
