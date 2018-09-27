#!/bin/bash
set -o errexit
set -o verbose

if [ $# -ne 3 ]; then
    echo "Arguments: <python command> <make flags> <config flags>"
    exit 3
fi

PYTHON=$1
OPENSSL_MAKE_FLAGS=$2
OPENSSL_CONFIG_FLAGS=$3

OPENSSL_VERSION=1.1.0i
OPENSSL_NAME=openssl-${OPENSSL_VERSION}
OPENSSL_TARBALL=${OPENSSL_NAME}.tar.gz
mkdir -p openssl
curl -L -o openssl/${OPENSSL_TARBALL} https://s3.amazonaws.com/boxes.10gen.com/build/${OPENSSL_TARBALL}
LOCAL_HASH="$(${PYTHON} buildscripts/sha256sum.py openssl/${OPENSSL_TARBALL})"
echo ${LOCAL_HASH}

if [ "$LOCAL_HASH" != "ebbfc844a8c8cc0ea5dc10b86c9ce97f401837f3fa08c17b2cdadc118253cf99" ]
then
    exit 3
fi

cd openssl
tar -xvzf ${OPENSSL_TARBALL} --strip-components 1
./config no-shared --prefix=${PWD}/../openssl_install_dir ${OPENSSL_CONFIG_FLAGS}
make ${OPENSSL_MAKE_FLAGS}
make install_sw
