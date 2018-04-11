#!/usr/bin/env bash

set -o verbose
set -o errexit

# This script downloads and import zlib
# Zlib does not need to use any autotools/cmake/config system to it is a simple import.
# This script is designed to run on most unix-like OSes
#

VERSION=1.2.11
NAME=zlib
TARBALL=${NAME}-${VERSION}.tar.gz
TARBALL_DEST_DIR=${NAME}-${VERSION}
DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/${NAME}-${VERSION}

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


# Generate the SConscript
( cat > ${DEST_DIR}/SConscript ) << ___EOF___
# -*- mode: python; -*-
Import("env")

env = env.Clone()

env.Append(CPPDEFINES=["HAVE_STDARG_H"])
if not env.TargetOSIs('windows'):
    env.Append(CPPDEFINES=["HAVE_UNISTD_H"])

env.Library(
    target="zlib",
    source=[
        'adler32.c',
        'crc32.c',
        'compress.c',
        'deflate.c',
        'infback.c',
        'inffast.c',
        'inflate.c',
        'inftrees.c',
        'trees.c',
        'uncompr.c',
        'zutil.c',
    ],
    LIBDEPS_TAGS=[
        'init-no-global-side-effects',
    ],
)
___EOF___

rm -f ${TARBALL}


# Note: There are no config.h or other build artifacts to generate
echo "Done"
