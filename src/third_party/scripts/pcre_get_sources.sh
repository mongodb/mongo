#!/bin/bash

# This script fetches and creates a copy of sources for pcre
# PCRE uses autotools on posix, and cmake on Windows
# PCRE has the same config.h file on Linux and Darwin, Solaris is unique
# To get the sources for PCRE, run this script as follows:
# 1. Run on Linux or Darwin
# 2. Run on Solaris
# 3. Run on Windows
#
VERSION=8.38
NAME=pcre
TARBALL=$NAME-$VERSION.tar.gz
TARBALL_DIR=$NAME-$VERSION
TEMP_DIR=/tmp/temp-$NAME-$VERSION
DEST_DIR=`git rev-parse --show-toplevel`/src/third_party/$NAME-$VERSION
UNAME=`uname | tr A-Z a-z`

if [ $UNAME == "sunos" ]; then
    TARGET_UNAME=solaris
elif [[ $UNAME == "cygwin"* ]]; then
    TARGET_UNAME=windows
else
    TARGET_UNAME=posix
fi

echo TARGET_UNAME: $TARGET_UNAME

if [ ! -f $TARBALL ]; then
    echo "Get tarball"
    wget ftp://ftp.csx.cam.ac.uk/pub/software/programming/pcre/$TARBALL
fi

tar -zxvf $TARBALL

rm -rf $TEMP_DIR
mv $TARBALL_DIR $TEMP_DIR
mkdir $DEST_DIR || true

cd $TEMP_DIR
if [ $TARGET_UNAME != "windows" ]; then

    # Do a shallow copy, it is all we need
    cp $TEMP_DIR/* $DEST_DIR
    rm -f $DEST_DIR/Makefile* $DEST_DIR/config* $DEST_DIR/*sh
    rm -f $DEST_DIR/compile* $DEST_DIR/depcomp $DEST_DIR/libtool
    rm -f $DEST_DIR/test-driver $DEST_DIR/*.m4 $DEST_DIR/missing

    echo "Generating Config.h and other files"
    ./configure --disable-jit --with-posix-malloc-threshold=10 --with-match-limit-recursion=4000 --disable-stack-for-recursion --with-link-size=2 -enable-newline-is-lf --with-match-limit=200000 --with-parens-nest-limit=250 --enable-utf --enable-unicode-properties --enable-shared=no 

    # We need to make it to get pcre_chartables.c
    make

    # Copy over the platform independent generated files
    cp $TEMP_DIR/pcre.h $DEST_DIR
    cp $TEMP_DIR/pcre_chartables.c $DEST_DIR
    cp $TEMP_DIR/pcre_stringpiece.h $DEST_DIR
    cp $TEMP_DIR/pcrecpparg.h $DEST_DIR

    # Copy over config.h
    mkdir $DEST_DIR/build_$TARGET_UNAME
    cp $TEMP_DIR/config.h $DEST_DIR/build_$TARGET_UNAME
else
    /cygdrive/c/Program\ Files\ \(x86\)/CMake/bin/cmake.exe -DPCRE_SUPPORT_PCREGREP_JIT:BOOL="0" -DPCRE_BUILD_TESTS:BOOL="0" -DPCRE_POSIX_MALLOC_THRESHOLD:STRING="10" -DPCRE_MATCH_LIMIT_RECURSION:STRING="4000"  -DPCRE_NO_RECURSE:BOOL="1" -DPCRE_LINK_SIZE:STRING="2" -DPCRE_NEWLINE:STRING="LF"  -DPCRE_MATCH_LIMIT:STRING="200000" -DPCRE_PARENS_NEST_LIMIT:STRING="250"  -DPCRE_SUPPORT_UTF:BOOL="1" -DPCRE_SUPPORT_UNICODE_PROPERTIES:BOOL="1"
fi

# Copy over config.h
mkdir $DEST_DIR/build_$TARGET_UNAME
cp $TEMP_DIR/config.h $DEST_DIR/build_$TARGET_UNAME

echo "Done"
