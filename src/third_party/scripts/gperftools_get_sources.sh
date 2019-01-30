#!/bin/bash
# This script downloads and imports gperftools.
# It can be run on Linux, Windows WSL or Mac OS X.
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

# convenience shell functions
function log2floor () { 
  local x=0 
  local y=$((($1)>>1))
  while [ $y -gt 0 ]; do 
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

# If WSL, get Windows temp directory
if $(grep -q Microsoft /proc/version); then
    TEMP_DIR=$(wslpath -u $(powershell.exe -Command "Get-ChildItem Env:TEMP | Get-Content | Write-Host"))
else
    TEMP_DIR="/tmp"
fi
TEMP_DIR=$(mktemp -d $TEMP_DIR/$NAME.XXXXXX)
trap "rm -rf $TEMP_DIR" EXIT

SRC_DIR=$TEMP_DIR/$NAME
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME-$VERSION
UNAME=$(uname | tr A-Z a-z)
UNAME_PROCESSOR=$(uname -p)

# Our build system chooses different names in this case so we need to match them
if [ $UNAME == "darwin" ]; then
    UNAME=osx
    UNAME_PROCESSOR=x86_64
fi

TARGET_UNAME=${UNAME}_${UNAME_PROCESSOR}

git clone https://github.com/mongodb-labs/gperftools.git -c core.autocrlf=false $TEMP_DIR

pushd $TEMP_DIR
git checkout $REVISION

./autogen.sh

if [ "$UNAME_PROCESSOR" = "ppc64le" ]; then
    PAGE_SIZE_KB=64
    MAX_SIZE_KB=64
else
    PAGE_SIZE_KB=4
    MAX_SIZE_KB=16
fi

env PATH=/opt/mongodbtoolchain/v3/bin:$PATH \
    ./configure \
    --enable-tcmalloc-aggressive-merge \
    --with-tcmalloc-pagesize=$PAGE_SIZE_KB \
    --with-tcmalloc-maxsize=$MAX_SIZE_KB

# Do a deep copy if this is the first time
if [ ! -d $DEST_DIR ]; then
    cp -r $TEMP_DIR $DEST_DIR || true

    DEST_CONFIG_DIR=$DEST_DIR/build_windows_x86_64
    mkdir $DEST_CONFIG_DIR
    sed "
    $(set_define TCMALLOC_ENABLE_LIBC_OVERRIDE 0)
    $(set_define TCMALLOC_AGGRESSIVE_MERGE 1)
    $(set_define TCMALLOC_PAGE_SIZE_SHIFT $(log2floor $((PAGE_SIZE_KB*1024))))
    $(set_define TCMALLOC_MAX_SIZE_KB ${MAX_SIZE_KB})
    " \
    < $TEMP_DIR/src/windows/config.h \
    > $DEST_CONFIG_DIR/config.h
fi

# Adjust config.h, See note 2 at top of file
mkdir $DEST_DIR/build_$TARGET_UNAME || true
cp src/config.h $DEST_DIR/build_$TARGET_UNAME/config.h

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

popd

# Prune sources
pushd $DEST_DIR
rm -rf autom4te.cache
rm -rf .git
rm -f src/config.h
rm -f compile*
rm -f depcomp 
rm -f libtool
rm -f test-driver 
rm -f *.m4 
rm -f missing
popd
