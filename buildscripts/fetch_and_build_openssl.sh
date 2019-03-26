#!/bin/bash
set -o errexit
#set -o verbose

if [ $# -ne 3 ]; then
    echo "Arguments: <python command> <make flags> <config flags>" >&2
    exit 3
fi

PYTHON="$1"   # not needed anymore
OPENSSL_MAKE_FLAGS="$2"
OPENSSL_CONFIG_FLAGS="$3"

OPENSSL_VERSION=1.1.1b
OPENSSL_NAME=openssl-${OPENSSL_VERSION}
OPENSSL_TARBALL=${OPENSSL_NAME}.tar.gz

basedir="$(pwd)"

mkdir -p openssl
pushd openssl
curl -L -o ${OPENSSL_TARBALL} \
    "https://s3.amazonaws.com/boxes.10gen.com/build/${OPENSSL_TARBALL}"

# To regenerate: shasum -a 256 -b $OPENSSL_TARBALL
shasum -c /dev/fd/0 <<EOF || exit 3
5c557b023230413dfb0756f3137a13e6d726838ccd1430888ad15bfb2b43ea4b *$OPENSSL_TARBALL
EOF

tar -xzf ${OPENSSL_TARBALL}

mkdir -p build
pushd build
../$OPENSSL_NAME/config no-shared --prefix="$basedir/openssl_install_dir" ${OPENSSL_CONFIG_FLAGS}
make ${OPENSSL_MAKE_FLAGS}
make install_sw
popd # build

popd # openssl
