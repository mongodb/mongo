#!/bin/bash
set -o verbose
set -o errexit

# This script fetches and creates a copy of sources for snappy
# snappy uses autotools on posix, and nothing on Windows
# Snappy has the same config.h file on Darwin and Solaris, Linux is unique
# The difference is byteswap.h
# To get the sources for Snappy, run this script as follows:
# 1. Run on Darwin or Solaris
# 2. Run on Linux

VERSION=1.1.3
NAME=snappy
TARBALL=$NAME-$VERSION.tar.gz
TARBALL_DIR=$NAME-$VERSION
TARBALL_DEST_DIR=$NAME-$VERSION
TEMP_DIR=/tmp/temp-$NAME-$VERSION
DEST_DIR=`git rev-parse --show-toplevel`/src/third_party/$NAME-$VERSION
UNAME=`uname | tr A-Z a-z`

if [ $UNAME == "linux" ]; then
    TARGET_UNAME=linux
else
    TARGET_UNAME=posix
fi

echo TARGET_UNAME: $TARGET_UNAME

if [ ! -f $TARBALL ]; then
    echo "Get tarball"
    wget https://github.com/google/$NAME/releases/download/$VERSION/$NAME-$VERSION.tar.gz
fi

echo $TARBALL
tar -zxvf $TARBALL

rm -rf $TEMP_DIR
mv $TARBALL_DIR $TEMP_DIR
mkdir $DEST_DIR || true

cd $TEMP_DIR
if [ $TARGET_UNAME != "windows" ]; then
    # Do a shallow copy, it is all we need
    cp $TEMP_DIR/* $DEST_DIR || true
    rm -f $DEST_DIR/Makefile* $DEST_DIR/config* $DEST_DIR/*sh
    rm -f $DEST_DIR/compile* $DEST_DIR/depcomp $DEST_DIR/libtool
    rm -f $DEST_DIR/test-driver $DEST_DIR/*.m4 $DEST_DIR/missing

    echo "Generating Config.h and other files"
    ./configure

    # Copy over the platform independent generated files
    cp $TEMP_DIR/snappy-stubs-public.h $DEST_DIR

    # Copy over config.h
    mkdir $DEST_DIR/build_$TARGET_UNAME
    cp $TEMP_DIR/config.h $DEST_DIR/build_$TARGET_UNAME
fi


echo "Done"
