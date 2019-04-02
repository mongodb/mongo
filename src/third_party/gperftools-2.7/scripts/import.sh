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

if [[ "$#" -ne 0 ]]; then
    echo "This script does not take any arguments" >&2
    exit 1
fi

NAME=gperftools
VERSION=2.7
REVISION=$VERSION-mongodb

# If WSL, get Windows temp directory
if $(grep -q Microsoft /proc/version); then
    TEMP_ROOT=$(wslpath -u $(powershell.exe -Command "Get-ChildItem Env:TEMP | Get-Content | Write-Host"))
else
    TEMP_ROOT="/tmp"
fi
REPO=$(mktemp -d $TEMP_ROOT/$NAME.XXXXXX)
trap "rm -rf $REPO" EXIT

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME-$VERSION
UNAME=$(uname | tr A-Z a-z)
UNAME_PROCESSOR=$(uname -m)

# Our build system chooses different names in this case so we need to match them
if [[ $UNAME == darwin ]]; then
    UNAME=osx
fi

TARGET_UNAME=${UNAME}_${UNAME_PROCESSOR}

git clone https://github.com/mongodb-forks/gperftools.git -c core.autocrlf=false $REPO

pushd $REPO
git checkout $REVISION
./autogen.sh

# configure just to generate a Makefile that has the 'make distdir' target.
./configure
DIST_DIR=$NAME-$VERSION
make distdir
make distclean

if [[ -d $DEST_DIR/dist ]]; then
    echo "You should 'rm -r $DEST_DIR/dist' before running import.sh" >&2
    exit 1
fi

mv $DIST_DIR $DEST_DIR/dist
popd
