#!/bin/bash

# This script fetches and creates a copy of sources for pcre2
# PCRE2 uses autotools on posix, and cmake on Windows
# PCRE2 has the same config.h file on Linux and Darwin
# To get the sources for PCRE2, run this script as follows:
# 1. Run on Linux or Darwin
# 2. Run on Windows

# Windows debugging
# If you see an error like
# CMake Error at CMakeLists.txt:103 (PROJECT):
#   Failed to run MSBuild command:
#
#     MSBuild.exe
#
#   to get the value of VCTargetsPath:
#
#     The system cannot find the file specified
#
# You are missing the path to MSBuild and you need to add something like this to the command line
# export PATH=$PATH:"/cygdrive/c/Program Files/Microsoft Visual Studio/2022/Community/Msbuild/Current/Bin"
set -euo pipefail
IFS=$'\n\t'

if [ "$#" -ne 0 ]; then
    echo "This script does not take any arguments"
    exit 1
fi

NAME=pcre2
SUFFIX=10.40
GIT_SSH=git@github.com:PCRE2Project/pcre2.git
THIRD_PARTY_DIR=$(git rev-parse --show-toplevel)/src/third_party
DEST_DIR=$THIRD_PARTY_DIR/$NAME

UNAME=$(uname | tr A-Z a-z)

if [[ $UNAME == "cygwin"* ]]; then
    # On windows absolute paths work weirdly
    TEMP_DIR=$(mktemp -d ./import-pcre2.XXXXXX)
    TARGET_UNAME=windows
else
    TEMP_DIR=$(mktemp -d /tmp/import-pcre2.XXXXXX)
    TARGET_UNAME=posix

fi

echo TARGET_UNAME: $TARGET_UNAME

git clone $GIT_SSH $TEMP_DIR
git -C $TEMP_DIR checkout $NAME-$SUFFIX -b master-$SUFFIX

rm -rf $TEMP_DIR/.git
rm -rf $THIRD_PARTY_DIR/pcre2*
mkdir -p $DEST_DIR

cd $TEMP_DIR
if [ $TARGET_UNAME != "windows" ]; then

    # All we need is the source code and the license
    mkdir $DEST_DIR/src
    (shopt -s dotglob; cp -r $TEMP_DIR/src/*  $DEST_DIR/src)
    cp $TEMP_DIR/LICENCE $DEST_DIR

    # We use autoconf to get ./configure
    aclocal
    autoreconf -i
    autoconf

    echo "Generating Config.h and other files"
    ./configure \
        --disable-jit \
        --with-match-limit-depth=4000 \
        --with-link-size=2 \
        --enable-newline-is-lf \
        --with-match-limit=200000 \
        --with-parens-nest-limit=250 \
        --enable-shared=no \
        CPPFLAGS="-DPCRE2_CODE_UNIT_WIDTH=8 -DHAVE_CONFIG_H"

    # We need to make it to get pcre2_chartables.c
    make

    # Copy over the platform independent generated files
    cp $TEMP_DIR/src/pcre2.h $DEST_DIR/src
    cp $TEMP_DIR/src/pcre2_chartables.c $DEST_DIR/src
else
    /cygdrive/c/cmake/bin/cmake.exe \
        -DPCRE2_SUPPORT_PCREGREP_JIT:BOOL="0" \
        -DPCRE2_BUILD_TESTS:BOOL="0" \
        -DPCRE2_MATCH_LIMIT_DEPTH:STRING="4000" \
        -DPCRE2_NO_RECURSE:BOOL="1" \
        -DPCRE2_LINK_SIZE:STRING="2" \
        -DPCRE2_NEWLINE:STRING="LF" \
        -DPCRE2_MATCH_LIMIT:STRING="200000" \
        -DPCRE2_PARENS_NEST_LIMIT:STRING="250"
fi

# Copy over config.h
mkdir $DEST_DIR/build_$TARGET_UNAME || true

if [ $TARGET_UNAME != "windows" ]; then
    cp $TEMP_DIR/src/config.h $DEST_DIR/build_$TARGET_UNAME
else
    cp config.h $DEST_DIR/build_$TARGET_UNAME
fi

echo Deleting $TEMP_DIR
rm -rf $TEMP_DIR
