#!/bin/bash
# This script downloads and imports gperftools.
# It can be run on Linux or Mac OS X.
# The actual integration via SConscript is not done by this script
#
# NOTES
# 1. Gperftools is autotools based except for Windows where it has a checked in config.h
# 2. On Linux, we generate config.h on the oldest supported distribution for each architecture
#    But to support newer distributions we must set some defines via SConscript instead of config.h
# 3. tcmalloc.h is configured by autotools for system installation purposes, but we modify it
#    to be used across platforms via an ifdef instead. This matches the corresponding logic used in
#    tcmalloc.cc to control functions that are guarded by HAVE_STRUCT_MALLINFO.

# Turn on strict error checking, like perl use 'strict'
set -euo pipefail
IFS=$'\n\t'

if [ "$#" -ne 0 ]; then
    echo "This script does not take any arguments"
    exit 1
fi

VERSION=2.5
NAME=gperftools
TARBALL=$NAME-$VERSION.tar.gz
TARBALL_DIR=$NAME-$VERSION
TEMP_DIR=$(mktemp -d /tmp/gperftools.XXXXXX)
trap "rm -rf $TEMP_DIR" EXIT
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME-$VERSION
UNAME=$(uname | tr A-Z a-z)
UNAME_PROCESSOR=$(uname -p)

# Our build system chooses different names in this case so we need to match them
if [ $UNAME == "darwin" ]; then
    UNAME=osx
    UNAME_PROCESSOR=x86_64
fi

TARGET_UNAME=${UNAME}_${UNAME_PROCESSOR}

if [ ! -f $TARBALL ]; then
    wget https://github.com/gperftools/gperftools/releases/download/$NAME-$VERSION/$NAME-$VERSION.tar.gz
fi

tar -zxvf $TARBALL

rm -rf $TEMP_DIR
mv $TARBALL_DIR $TEMP_DIR

# Do a deep copy if this is the first time
if [ ! -d $DEST_DIR ]; then
    cp -r $TEMP_DIR $DEST_DIR || true

    # Copy over the Windows header to our directory structure
    mkdir $DEST_DIR/build_windows_x86_64
    cp $TEMP_DIR/src/windows/config.h $DEST_DIR/build_windows_x86_64
fi

# Generate Config.h & tcmalloc.h
cd $TEMP_DIR
./configure

# Adjust config.h, See note 2 at top of file
mkdir $DEST_DIR/build_$TARGET_UNAME || true
sed "s/.*MALLOC_HOOK_MAYBE_VOLATILE.*/\/* #undef MALLOC_HOOK_MAYBE_VOLATILE *\//" < src/config.h \
    > $DEST_DIR/build_$TARGET_UNAME/config.h

# Generate tcmalloc.h
# See note 3 at top of file
if [ ! -d $DEST_DIR/src/gperftools/tcmalloc.h ]; then
    TCMALLOC_H=$DEST_DIR/src/gperftools/tcmalloc.h
    TCMALLOC_H_IN=src/gperftools/tcmalloc.h.in
    TCMALLOC_H_TMP=tcmalloc.h.bak
    cp src/gperftools/tcmalloc.h $TCMALLOC_H

    # Change the autotools subsitution into an ifdef instead
    for line_number in $(grep -n "@ac_cv_have_struct_mallinfo@" $TCMALLOC_H_IN | cut -d: -f1) ; do
        sed "${line_number}s/.*/#ifdef HAVE_STRUCT_MALLINFO/" < $TCMALLOC_H > $TCMALLOC_H_TMP
        cp $TCMALLOC_H_TMP $TCMALLOC_H
    done
fi

# Prune sources
cd $DEST_DIR
rm -rf $DEST_DIR/benchmark
rm -rf $DEST_DIR/doc
rm -rf $DEST_DIR/m4
rm -rf $DEST_DIR/packages
rm -rf $DEST_DIR/src/tests
rm -rf $DEST_DIR/vsprojects
rm -f $DEST_DIR/Makefile* $DEST_DIR/config* $DEST_DIR/*sh
rm -f $DEST_DIR/compile* $DEST_DIR/depcomp $DEST_DIR/libtool
rm -f $DEST_DIR/test-driver $DEST_DIR/*.m4 $DEST_DIR/missing
rm -f $DEST_DIR/*.sln
