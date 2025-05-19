#!/bin/sh
# Copyright (C) The c-ares project and its contributors
# SPDX-License-Identifier: MIT
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

if [ "$BUILD_TYPE" = "autotools" -o "$BUILD_TYPE" = "coverage" ]; then
    autoreconf -fi
    mkdir atoolsbld
    cd atoolsbld
    if [ "$DIST" = "iOS" ] ; then
        export CFLAGS="${CFLAGS} -isysroot ${SYSROOT}"
        export CXXFLAGS="${CXXFLAGS} -isysroot ${SYSROOT}"
        export LDFLAGS="${LDFLAGS} -isysroot ${SYSROOT}"
    fi
    export CFLAGS="${CFLAGS} -O0 -g"
    export CXXFLAGS="${CXXFLAGS} -O0 -g"
    $SCAN_WRAP ../configure --disable-symbol-hiding --enable-maintainer-mode $CONFIG_OPTS
    $SCAN_WRAP make
else
    # Use cmake for everything else
    if [ "$DIST" = "iOS" ] ; then
        CMAKE_FLAGS="${CMAKE_FLAGS} -DCMAKE_OSX_SYSROOT=${SYSROOT}"
    fi
    $SCAN_WRAP cmake ${CMAKE_FLAGS} ${CMAKE_TEST_FLAGS} -Bcmakebld .
    $SCAN_WRAP cmake --build cmakebld
fi
