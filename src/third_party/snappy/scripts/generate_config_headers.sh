#!/bin/bash

# This script builds and installs the headers needed for snappy.
# snappy uses CMake and this script will invoke CMake to generate a
# config.h for "posix", Linux, and Windows
#
# To get the sources for Snappy, run this script as follows:
#
# 1. Run on Darwin
# 2. Run on Windows via Cygwin
# 3. Run on all Linux archs (aarch64, x86, s390x, ppc64le)
#
# For Windows you will need CMake installed. If using an
# Evergreen spawn host it is not installed by default but, you can
# easily install it with the following command. (this works in
# cygwin):
#
# choco install cmake
#
# You will also need to add it to the $PATH with the following:
#
# export PATH="/cygdrive/c/Program Files/CMake/bin/:$PATH"

set -o verbose
set -o errexit

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/snappy
TEMP_DIR=/tmp/temp-snappy

UNAME=`uname | tr A-Z a-z`
UNAME_PROCESSOR=$(uname -m)

if [ "$UNAME" == "linux" ]; then
    TARGET_UNAME=linux_${UNAME_PROCESSOR}
    export CC=/opt/mongodbtoolchain/v4/bin/gcc
    export CXX=/opt/mongodbtoolchain/v4/bin/g++
elif [[ "$UNAME" == "cygwin"* ]]; then
    TARGET_UNAME=windows
else
    TARGET_UNAME=posix
fi

echo TARGET_UNAME: $TARGET_UNAME

# build the config files
mkdir $TEMP_DIR || true
cp -r $DEST_DIR/dist/ $TEMP_DIR/

echo "Generating Config.h and other files"
cmake -DSNAPPY_BUILD_TESTS=OFF -DSNAPPY_BUILD_BENCHMARKS=OFF -B$TEMP_DIR -H$DEST_DIR/dist

# Move over the platform independent generated files
mkdir $DEST_DIR/platform/build_all || true
mv $TEMP_DIR/snappy-stubs-public.h $DEST_DIR/platform/build_all
pushd $DEST_DIR/platform/build_all
# Change the snappy-stubs-public.h to use the defined variables
# instead of hardcoded values generated by CMake
#
# Changes lines like:
#
#    #if !0  // !HAVE_SYS_UIO_H
#    #if 1  // HAVE_STDINT_H
#
# To:
#
#    #if !HAVE_SYS_UIO_H
#    #if HAVE_STDINT_H
if [ "$TARGET_UNAME" = "posix" ]; then
    sed -i '' 's/if !\{0,1\}[0-9]  \/\/ \(!\{0,1\}HAVE_.*\)/if \1/' snappy-stubs-public.h
else
    sed -i 's/if !\{0,1\}[0-9]  \/\/ \(!\{0,1\}HAVE_.*\)/if \1/' snappy-stubs-public.h
fi
popd

# Move over config.h
mkdir $DEST_DIR/platform/build_$TARGET_UNAME || true
mv $TEMP_DIR/config.h $DEST_DIR/platform/build_$TARGET_UNAME

echo "Done building config files"

# clean up
echo "Cleaning up"
rm -rf $TEMP_DIR

echo "Done"
