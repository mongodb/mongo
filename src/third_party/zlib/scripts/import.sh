#!/usr/bin/env bash

set -o verbose
set -o errexit

# This script downloads and import zlib
# Zlib does not need to use any autotools/cmake/config system to it is a simple import.
# This script is designed to run on most unix-like OSes
#

VERSION=1.3.2
NAME=zlib
TARBALL=${NAME}-${VERSION}.tar.gz
TARBALL_DEST_DIR=${NAME}-${VERSION}
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/${NAME}

echo ${DEST_DIR}

rm -fr ${TARBALL_DEST_DIR}
rm -f ${TARBALL}

if [ ! -f ${TARBALL} ]; then
    echo "Get tarball"
    wget https://www.zlib.net/${TARBALL}
fi

tar -zxvf ${TARBALL}

rm -rf ${DEST_DIR}
mkdir ${DEST_DIR}

# Just move the sources
mv ${TARBALL_DEST_DIR}/*.{h,c} ${DEST_DIR}

# Move the readme and such.
mv ${TARBALL_DEST_DIR}/{README,FAQ,INDEX} ${DEST_DIR}

rm -fR ${TARBALL_DEST_DIR}

rm -f ${TARBALL}


# Note: There are no config.h or other build artifacts to generate
echo "Done"
