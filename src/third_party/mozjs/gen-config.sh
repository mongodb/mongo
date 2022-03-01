#!/bin/bash

set -e
set -v
set -x

if [ $# -ne 2 ]
then
    echo "Please supply an arch: x86_64, i386, etc and a platform: osx, linux, windows, etc"
    exit 0;
fi

_BuiltPathPrefix="mozilla-release/js/src/_build/js/src"
_Path=platform/$1/$2
shift
shift

_CONFIG_OPTS=""

_xcode_setup() {
    local sdk=$1; shift
    local arch=$1; shift
    local target=$1; shift
    export SDKROOT=`xcrun --sdk $sdk --show-sdk-path`
    export HOST_CC=/usr/bin/gcc
    export HOST_CXX=/usr/bin/c++
    export CC=`xcrun -f clang`" -arch $arch -isysroot $SDKROOT -m$target"
    export CXX=`xcrun -f clang++`" -arch $arch -isysroot $SDKROOT -m$target"
}

case "$_Path" in
    "platform/aarch64/linux")
        _CONFIG_OPTS="--host=aarch64-linux"
    ;;
    "platform/ppc64le/freebsd")
        _CONFIG_OPTS="--host=ppc64le-freebsd"
    ;;
    "platform/ppc64le/linux")
        _CONFIG_OPTS="--host=ppc64le-linux"
    ;;
    "platform/s390x/linux")
        _CONFIG_OPTS="--host=s390x-linux"
    ;;
    "platform/x86_64/freebsd")
        _CONFIG_OPTS="--host=x86_64-freebsd"
    ;;
    "platform/x86_64/linux")
        _CONFIG_OPTS="--host=x86_64-linux"
    ;;
    "platform/x86_64/openbsd")
        _CONFIG_OPTS="--host=x86_64-openbsd"
    ;;
    "platform/x86_64/windows")
        _CONFIG_OPTS="--host=x86_64-windows"
    ;;
    "platform/aarch64/macOS")
        _xcode_setup "macosx" "arm64" "macos-version-min=10.9"
        _CONFIG_OPTS="--host=aarch64-apple-darwin"
    ;;
    "platform/x86_64/macOS")
        _xcode_setup "macosx" "x86_64" "macos-version-min=10.9"
        _CONFIG_OPTS="--host=x86_64-apple-darwin"
    ;;
    "platform/aarch64/iOS")
        _xcode_setup "iphoneos" "arm64" "iphoneos-version-min=10.2"
        _CONFIG_OPTS="--target=aarch64-apple-darwin"
        ;;
    "platform/x86_64/iOS-sim")
        _xcode_setup "iphonesimulator" "x86_64" "iphoneos-version-min=10.2"
        _CONFIG_OPTS="--host=x86_64-apple-darwin"
        ;;
    "platform/aarch64/tvOS")
        _xcode_setup "appletvos" "arm64" "tvos-version-min=10.1"
        _CONFIG_OPTS="--target=aarch64-apple-darwin"
        ;;
    "platform/x86_64/tvOS-sim")
        _xcode_setup "appletvsimulator" "x86_64" "tvos-version-min=10.1"
        _CONFIG_OPTS="--host=x86_64-apple-darwin"
        ;;
    *)
        echo "Unknown configuration $_Path"
        exit 1
        ;;
esac

# the two files we need are js-confdefs.h which get used for the build and
# js-config.h for library consumers.  We also get different unity source files
# based on configuration, so save those too.

cd mozilla-release/js/src

echo "Create _build"
rm -rf _build
mkdir -p _build
cd _build

rm config.cache || true

echo "Run configure"
# The 'ppc64le/linux' platform requires the additional 'CXXFLAGS' and 'CFLAGS' flags to compile
CXXFLAGS="$CXXFLAGS -D__STDC_FORMAT_MACROS"\
CFLAGS="$CFLAGS -D__STDC_FORMAT_MACROS" \
../configure \
    --disable-jemalloc \
    --with-system-zlib \
    --without-intl-api \
    --enable-optimize \
    --disable-js-shell \
    --disable-tests "$_CONFIG_OPTS"

make recurse_export

cd ../../../..

rm -rf $_Path/

mkdir -p $_Path/build
mkdir -p $_Path/include
cp $_BuiltPathPrefix/*.cpp $_Path/build
cp $_BuiltPathPrefix/*.h $_Path/include
cp $_BuiltPathPrefix/js-confdefs.h $_Path/build

mkdir -p $_Path/build/jit
mkdir -p $_Path/include/jit
cp $_BuiltPathPrefix/jit/*.cpp $_Path/build/jit
cp $_BuiltPathPrefix/jit/*.h $_Path/include/jit


mkdir -p $_Path/build/gc
mkdir -p $_Path/include/gc
cp $_BuiltPathPrefix/gc/*.cpp $_Path/build/gc
cp $_BuiltPathPrefix/gc/*.h $_Path/include/gc

mkdir -p $_Path/build/wasm
mkdir -p $_Path/include/wasm
cp $_BuiltPathPrefix/wasm/*.cpp $_Path/build/wasm

mkdir -p $_Path/build/irregexp
mkdir -p $_Path/include/irregexp
cp $_BuiltPathPrefix/irregexp/*.cpp $_Path/build/irregexp


mkdir -p $_Path/build/debugger
mkdir -p $_Path/include/debugger
cp $_BuiltPathPrefix/debugger/*.cpp $_Path/build/debugger

mkdir -p $_Path/build/frontend
mkdir -p $_Path/include/frontend
cp $_BuiltPathPrefix/frontend/*.cpp $_Path/build/frontend
cp $_BuiltPathPrefix/frontend/*.h $_Path/include/frontend

cp $_BuiltPathPrefix/js-config.h $_Path/include

SEDOPTION="-i"
if [[ "$OSTYPE" == "darwin"* ]]; then
  SEDOPTION="-i ''"
fi

find "$_Path/build" -name '*.cpp' |
    while read unified_file ; do
        echo "Processing $unified_file"
        sed $SEDOPTION \
            -e 's|#include ".*/js/src/|#include "|' \
            -e 's|#error ".*/js/src/|#error "|' \
            "$unified_file"
  done
