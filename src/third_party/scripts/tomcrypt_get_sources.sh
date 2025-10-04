#!/bin/sh

set -o verbose
set -o errexit

set -euo pipefail
IFS=$'\n\t'

if [ "$#" -ne 0 ]; then
    echo "This script does not take any arguments"
    exit 1
fi

# how we got the last tom crypt sources

VERSION=1.18.2

cd `git rev-parse --show-toplevel`/src/third_party/tomcrypt-$VERSION

TARBALL=crypt-$VERSION.tar.xz
if [ ! -f $TARBALL ]; then
    wget "https://github.com/libtom/libtomcrypt/releases/download/v$VERSION/$TARBALL"
fi

xzcat $TARBALL | tar -xf-

rm $TARBALL

mv libtomcrypt-$VERSION libtomcrypt-release

rm -rf src

mkdir src
mkdir src/ciphers
mkdir src/hashes
mkdir src/hashes/helper
mkdir src/hashes/sha2
mkdir src/mac
mkdir src/mac/hmac
mkdir src/misc
mkdir src/misc/crypt
mkdir src/modes
mkdir src/modes/ctr
mkdir src/modes/cbc

FILES=(
    hashes/helper/hash_memory.c
    hashes/md5.c
    hashes/sha1.c
    hashes/sha2/sha256.c
    hashes/sha2/sha512.c
    mac/hmac/hmac_done.c
    mac/hmac/hmac_init.c
    mac/hmac/hmac_memory.c
    mac/hmac/hmac_process.c
    misc/compare_testvector.c
    misc/crypt/crypt_argchk.c
    misc/crypt/crypt_cipher_is_valid.c
    misc/crypt/crypt_cipher_descriptor.c
    misc/crypt/crypt_find_cipher.c
    misc/crypt/crypt_find_hash.c
    misc/crypt/crypt_hash_descriptor.c
    misc/crypt/crypt_hash_is_valid.c
    misc/crypt/crypt_register_cipher.c
    misc/crypt/crypt_register_hash.c
    misc/zeromem.c
    modes/cbc/cbc_done.c
    modes/cbc/cbc_start.c
    modes/cbc/cbc_encrypt.c
    modes/cbc/cbc_decrypt.c
    modes/ctr/ctr_done.c
    modes/ctr/ctr_start.c
    modes/ctr/ctr_encrypt.c
    modes/ctr/ctr_decrypt.c
    )

cp -r libtomcrypt-release/src/headers src
cp -r libtomcrypt-release/src/ciphers/aes src/ciphers

for file in "${FILES[@]}"
    do cp libtomcrypt-release/src/$file src/$file
done
