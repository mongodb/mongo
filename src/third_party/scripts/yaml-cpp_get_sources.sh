#!/bin/bash

set -o verbose
set -o errexit

# This script downloads and import yaml-cpp
# Yaml-cpp does not use any autotools/cmake/config system to it is a simple import.
# This script is designed to run on Linux or Mac OS X
#
# Yaml-cpp tarballs use the name "yaml-cpp-release" so we need to rename it
#

VERSION=0.5.3
NAME=yaml-cpp
TARBALL=release-$VERSION.tar.gz
TARBALL_DEST_DIR=$NAME-release-$VERSION
DEST_DIR=`git rev-parse --show-toplevel`/src/third_party/$NAME-$VERSION

if [ ! -f $TARBALL ]; then
    echo "Get tarball"
    wget https://github.com/jbeder/yaml-cpp/archive/$TARBALL
fi

tar -zxvf $TARBALL

rm -rf $DEST_DIR

mv $TARBALL_DEST_DIR $DEST_DIR

# Prune sources
echo "Prune tree"
rm -rf $DEST_DIR/test
rm -rf $DEST_DIR/util
rm -f $DEST_DIR/CMakeLists.txt
rm -f $DEST_DIR/*.cmake*

# Note: There are no config.h or other build artifacts to generate
echo "Done"
