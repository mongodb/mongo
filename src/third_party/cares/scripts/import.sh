#!/bin/bash
# This script downloads and imports cares

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=cares
REVISION="cares-1_27_0"
VERSION="1.27.0"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/cares
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

git clone --branch $REVISION git@github.com:mongodb-forks/c-ares.git $DEST_DIR/dist

HOST_OS="$(uname -s|tr A-Z a-z)"
HOST_ARCH="$(uname -m)"
HOST_DIR="$DEST_DIR/platform/${HOST_OS}_${HOST_ARCH}"

TOOLCHAIN_ROOT=/opt/mongodbtoolchain/v4
PATH="$TOOLCHAIN_ROOT/bin:$PATH"

SRC_DIR=${DEST_DIR}/dist
pushd $SRC_DIR
autoreconf -fi
popd

mkdir -p $HOST_DIR/build_tmp
pushd $HOST_DIR/build_tmp

$SRC_DIR/configure \
    --prefix=$HOST_DIR/install_tmp \
    CC=$TOOLCHAIN_ROOT/bin/gcc \
    CXX=$TOOLCHAIN_ROOT/bin/g++


CC=$TOOLCHAIN_ROOT/bin/gcc
CXX=$TOOLCHAIN_ROOT/bin/g++

make -j"$(grep -c ^processor /proc/cpuinfo)" CC=$CC CXX=$CXX install
popd

pushd $DEST_DIR/dist
# tranfer useful gen files
mkdir -p $HOST_DIR/build/include
mkdir -p $HOST_DIR/install/include

# Config artifacts needed to rebuild the library.
config_headers=(
    ares_config.h
)

# Headers needed to use the library.
public_headers=(
    ares_build.h
    ares_dns.h
    ares_dns_record.h
    ares_nameser.h
    ares_rules.h
    ares_version.h
    ares.h
)

mkdir -p "$HOST_DIR/build/include"
for h in "${config_headers[@]}" ; do
    cp "$HOST_DIR/build_tmp/src/lib/$h" "$HOST_DIR/build/include/$h"
done

mkdir -p "$HOST_DIR/install/include"
for h in "${public_headers[@]}" ; do
    cp "$HOST_DIR/install_tmp/include/$h" "$HOST_DIR/install/include/$h"
done

# clean up
find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;
rm -rf $HOST_DIR/build_tmp
rm -rf $HOST_DIR/install_tmp
rm -rf *.cache
rm -rf travis
rm -rf *.tar.gz
rm -rf test
rm -rf m4
rm -rf docs
find . -type f -name "Makefile*" -exec rm -rf {} \;
popd


