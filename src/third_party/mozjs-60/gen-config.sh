#!/bin/sh

set -e
set -v
set -x

if [ $# -ne 2 ]
then
    echo "Please supply an arch: x86_64, i386, etc and a platform: osx, linux, windows, etc"
    exit 0;
fi

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
rm config.cache || true

PYTHON=python ./configure --without-intl-api --enable-posix-nspr-emulation --disable-trace-logging --disable-js-shell --disable-tests "$_CONFIG_OPTS"

make recurse_export

cd ../../..

rm -rf $_Path/

mkdir -p $_Path/build
mkdir $_Path/include

cp mozilla-release/js/src/js/src/js-confdefs.h $_Path/build
cp mozilla-release/js/src/js/src/*.cpp $_Path/build
cp mozilla-release/js/src/js/src/js-config.h $_Path/include

for unified_file in $(ls -1 $_Path/build/*.cpp) ; do
	sed 's/#include ".*\/js\/src\//#include "/' < $unified_file > t1
	sed 's/#error ".*\/js\/src\//#error "/' < t1 > $unified_file
	rm t1
done


