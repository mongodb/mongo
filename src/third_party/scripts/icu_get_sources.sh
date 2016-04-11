#!/bin/bash
set -o verbose
set -o errexit

# This script fetches and creates a copy of sources for ICU.

NAME=icu4c
MAJOR_VERSION=57
MINOR_VERSION=1
VERSION=${MAJOR_VERSION}.${MINOR_VERSION}

TARBALL=$NAME-$MAJOR_VERSION\_$MINOR_VERSION-src.tgz
TARBALL_DIR=icu
TARBALL_DEST_DIR=$NAME-$VERSION
TARBALL_DOWNLOAD_URL=http://download.icu-project.org/files/$NAME/$VERSION/$TARBALL

TEMP_DIR=/tmp/temp-$NAME-$VERSION
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/$NAME-$VERSION

# Download and extract tarball.
if [ ! -f $TARBALL ]; then
    echo "Get tarball"
    wget $TARBALL_DOWNLOAD_URL
fi

echo $TARBALL
tar -zxvf $TARBALL

# Move extracted files to a temporary directory.
rm -rf $TEMP_DIR
mv $TARBALL_DIR $TEMP_DIR

# If the SConscript for building ICU already exists, move it into the temporary directory.
if [ -f $DEST_DIR/source/SConscript ]; then
    echo "Saving SConscript"
    mv $DEST_DIR/source/SConscript $TEMP_DIR/source
    rm -rf $DEST_DIR
fi

# Copy all sources into their proper place in the mongo source tree.
if [ ! -d $DEST_DIR ]; then
    mkdir $DEST_DIR
fi

cp -r $TEMP_DIR/* $DEST_DIR || true

# Prune sources.
rm -f $DEST_DIR/source/*.in             # Build system.
rm -f $DEST_DIR/source/*.m4             # Build system.
rm -f $DEST_DIR/source/install-sh       # Build system.
rm -f $DEST_DIR/source/mkinstalldirs    # Build system.
rm -f $DEST_DIR/source/runConfigureICU  # Build system.
rm -rf $DEST_DIR/as_is/                 # Scripts.
rm -rf $DEST_DIR/source/allinone/       # Workspace and project files.
rm -rf $DEST_DIR/source/config*         # Build system.
rm -rf $DEST_DIR/source/data/           # Source data.
rm -rf $DEST_DIR/source/extra/          # Non-supported API additions.
rm -rf $DEST_DIR/source/io/             # ICU I/O library.
rm -rf $DEST_DIR/source/layout/         # ICU complex text layout engine.
rm -rf $DEST_DIR/source/layoutex/       # ICU paragraph layout engine.
rm -rf $DEST_DIR/source/samples/        # Sample programs.
rm -rf $DEST_DIR/source/test/           # Test suites.
rm -rf $DEST_DIR/source/tools/          # Tools for generating the data files.

echo "Done"
