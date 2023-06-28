#!/bin/sh
set -e

OS=""
if [ "$TRAVIS_OS_NAME" != "" ]; then
    OS="$TRAVIS_OS_NAME"
elif [ "$CIRRUS_OS" != "" ]; then
    OS="$CIRRUS_OS"
fi

if [ "$DIST" = "iOS" ] ; then
   XCODE_PATH=`xcode-select -print-path`
   SYSROOT="${XCODE_PATH}/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk/"
fi

if [ "$BUILD_TYPE" != "cmake" -a "$BUILD_TYPE" != "valgrind" ]; then
    autoreconf -fi
    mkdir atoolsbld
    cd atoolsbld
    if [ "$DIST" = "iOS" ] ; then
        export CFLAGS="${CFLAGS} -isysroot ${SYSROOT}"
        export CXXFLAGS="${CXXFLAGS} -isysroot ${SYSROOT}"
        export LDFLAGS="${LDFLAGS} -isysroot ${SYSROOT}"
    fi
    $SCAN_WRAP ../configure --disable-symbol-hiding --enable-expose-statics --enable-maintainer-mode --enable-debug $CONFIG_OPTS
    $SCAN_WRAP make
else
    # Use cmake for valgrind to prevent libtool script wrapping of tests that interfere with valgrind
    mkdir cmakebld
    cd cmakebld
    if [ "$DIST" = "iOS" ] ; then
        CMAKE_FLAGS="${CMAKE_FLAGS} -DCMAKE_OSX_SYSROOT=${SYSROOT}"
    fi
    cmake ${CMAKE_FLAGS} ..
    make
fi
