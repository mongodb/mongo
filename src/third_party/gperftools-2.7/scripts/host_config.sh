#!/bin/bash

set -euo pipefail
IFS=$'\n\t'

# convenience shell functions
function log2floor () {
  local x=0
  local y=$((($1)>>1))
  while [[ $y -gt 0 ]]; do
    x=$((x+1))
    y=$((y>>1))
  done
  echo $x
}

function set_define () {
    # change any line matching the macro name, surrounded by spaces,
    # to be a #define of that macro to the specified value.
    echo "/ $1 /c\\"
    echo "#define $1 $2"
}

NAME=gperftools
VERSION=2.7
REVISION=$VERSION-mongodb
MACOSX_VERSION_MIN=10.12

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME-$VERSION

UNAME=$(uname | tr A-Z a-z)
UNAME_PROCESSOR=$(uname -m)

# Our build system chooses different names in this case so we need to match them
if [[ $UNAME == darwin ]]; then
    UNAME=osx
fi

TARGET_UNAME=${UNAME}_${UNAME_PROCESSOR}

PAGE_SIZE_KB=4
MAX_SIZE_KB=16
TARGET_TRANSFER_KB=8

WINDOWS_PAGE_SIZE_KB=$PAGE_SIZE_KB
WINDOWS_MAX_SIZE_KB=$MAX_SIZE_KB
WINDOWS_TARGET_TRANSFER_KB=$TARGET_TRANSFER_KB

if [[ $UNAME_PROCESSOR == ppc64le ]]; then
    PAGE_SIZE_KB=64
    MAX_SIZE_KB=64
fi

COMMON_FLAGS="-g -O2"   # configure's defaults for both C and C++

if [[ $UNAME == osx ]]; then
    COMMON_FLAGS+=" -mmacosx-version-min=$MACOSX_VERSION_MIN"
else
    ENV_CPPFLAGS="CPPFLAGS=-D_XOPEN_SOURCE=700 -D_GNU_SOURCE"
fi

ENV_CFLAGS="CFLAGS=$COMMON_FLAGS"
ENV_CXXFLAGS="CXXFLAGS=$COMMON_FLAGS -std=c++17"

PLATFORM_DIR="$DEST_DIR/platform"
HOST_CONFIG="$PLATFORM_DIR/$TARGET_UNAME"

mkdir -p $HOST_CONFIG/internal
pushd $HOST_CONFIG/internal

PATH=/opt/mongodbtoolchain/v3/bin:$PATH
env \
    ${ENV_CPPFLAGS:-} \
    ${ENV_CFLAGS:-} \
    ${ENV_CXXFLAGS:-} \
    $DEST_DIR/dist/configure \
        --prefix=$HOST_CONFIG/junk \
        --includedir=$HOST_CONFIG/include \
        --enable-frame-pointers=yes \
        --enable-libunwind=no \
        --enable-sized-delete=yes \
        --enable-tcmalloc-aggressive-merge \
        --enable-tcmalloc-mallinfo=no \
        --enable-tcmalloc-unclamped-transfer-sizes=yes \
        --enable-tcmalloc-target-transfer-kb=$TARGET_TRANSFER_KB \
        --with-tcmalloc-pagesize=$PAGE_SIZE_KB \
        --with-tcmalloc-maxsize=$MAX_SIZE_KB

# Make sure our configuration makes sense to tcmalloc's tests.
# Note that sampling_test.sh has occasionally shown a flaky failure.
if [[ ${TCMALLOC_CHECK:-0} == 1 ]]; then
    make check
fi

# Compose a consumer-side install of just the include dir.
# This also produces other installed junk (docs, pkg-config), which we discard.
make install-data
rm -rf "$HOST_CONFIG/junk"
make clean
popd  # $HOST_CONFIG/internal

# Pseudo-configure Windows.
function configure_windows() {
    local WINDOWS_CONFIG_DIR="$PLATFORM_DIR/windows_x86_64"
    # Editing with sed is the best we can reasonably do, as gperftools doesn't
    # ship with a Windows configuration mechanism.
    local WINDOWS_CONFIG_H="$WINDOWS_CONFIG_DIR/internal/src/config.h"
    mkdir -p "$(dirname "$WINDOWS_CONFIG_H")"
    sed "
    $(set_define TCMALLOC_ENABLE_LIBC_OVERRIDE 0)
    $(set_define TCMALLOC_AGGRESSIVE_MERGE 1)
    $(set_define TCMALLOC_PAGE_SIZE_SHIFT $(log2floor $((WINDOWS_PAGE_SIZE_KB*1024))))
    $(set_define TCMALLOC_MAX_SIZE_KB ${WINDOWS_MAX_SIZE_KB})
    $(set_define TCMALLOC_TARGET_TRANSFER_KB ${WINDOWS_TARGET_TRANSFER_KB})
    $(set_define TCMALLOC_USE_UNCLAMPED_TRANSFER_SIZES 1)
    " \
    < "$DEST_DIR/dist/src/windows/config.h" \
    > "$WINDOWS_CONFIG_H"

    # gperftools ships with a src/windows/gperftools/tcmalloc.h as well the tcmalloc.h.in file
    # from which it is generated. It is also regenerated as part of './configure'.
    # This means a non-overwriting configure will generate a competing tcmalloc.h file to the
    # one gperftools provides. We should make sure our system include path has exactly one
    # <gperftools/tcmalloc.h> when building on Windows.
    local WINDOWS_TCMALLOC_H_RELEASED="$DEST_DIR/dist/src/windows/gperftools/tcmalloc.h"
    local WINDOWS_TCMALLOC_H="$WINDOWS_CONFIG_DIR/internal/src/gperftools/tcmalloc.h"
    mkdir -p "$(dirname "$WINDOWS_TCMALLOC_H")"
    cp "$WINDOWS_TCMALLOC_H_RELEASED" "$WINDOWS_TCMALLOC_H"

    # Completely synthesize a Windows include/gperftools directory.
    # Guess: copying *this* platform's include/ and overwriting gperftools/tcmalloc.h with
    # the tcmalloc.h from the Google-provided windows/ header.
    cp -a "$HOST_CONFIG/include" "$WINDOWS_CONFIG_DIR/include"
    cp "$WINDOWS_TCMALLOC_H" "$WINDOWS_CONFIG_DIR/include/gperftools/tcmalloc.h"
}

configure_windows
