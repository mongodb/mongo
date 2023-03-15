#!/bin/bash
# This script downloads and imports cares

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=cares
REVISION="cares-1_17_2"
VERSION="1.17.2"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/cares
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

git clone --branch cares-1_17_2 git@github.com:mongodb-forks/c-ares.git $DEST_DIR/dist

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
# mkdir -p $HOST_DIR/build/include
# mkdir -p $HOST_DIR/install/include
# cp $HOST_DIR/build_tmp/src/lib/ares_config.h $HOST_DIR/build/include/ares_config.h
# cp $HOST_DIR/install_tmp/include/ares_build.h $HOST_DIR/install/include/ares_build.h
# cp $HOST_DIR/install_tmp/include/ares_dns.h $HOST_DIR/install/include/ares_dns.h
# cp $HOST_DIR/install_tmp/include/ares_rules.h $HOST_DIR/install/include/ares_rules.h
# cp $HOST_DIR/install_tmp/include/ares_version.h $HOST_DIR/install/include/ares_version.h
# cp $HOST_DIR/install_tmp/include/ares.h $HOST_DIR/install/include/ares.h

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


