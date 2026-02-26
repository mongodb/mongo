#! /bin/bash
#
# Copyright (c) 2023 [Ribose Inc](https://www.ribose.com).
# All rights reserved.
# This file is a part of rnp
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
# TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
# BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

set -o errexit -o pipefail -o noclobber -o nounset

DIR0="$( cd "$( dirname "$0" )" && pwd )"
# shellcheck disable=SC2034
SHUNIT_PARENT="$0"

# Defaults applicable to 'normal' installation and not build environment
: "${BOTAN_INSTALL:=/usr}"
: "${JSONC_INSTALL:=/usr}"
: "${RNP_INSTALL:=/usr}"

: "${ENABLE_SM2:=}"
: "${ENABLE_IDEA:=}"

test_symbol_visibility() {
    case "$OSTYPE" in
      msys|cygwin)
        mkdir tmp
        wget -O tmp/Dependencies_x64_Release.zip https://github.com/lucasg/Dependencies/releases/download/v1.10/Dependencies_x64_Release.zip
        7z x tmp/Dependencies_x64_Release.zip -otmp
        tmp/Dependencies -exports "$RNP_INSTALL"/bin/librnp.dll  > exports
        rm -rf tmp
        ;;
      darwin*)
        nm --defined-only -g "$RNP_INSTALL"/lib/librnp.dylib > exports
        ;;
      *)
        nm --defined-only -g "$RNP_INSTALL"/lib64/librnp*.so > exports
    esac

    assertEquals "Unexpected: 'dst_close' is in exports" 0 "$(grep -c dst_close exports)"
    assertEquals "Unexpected: 'Botan' is in exports" 0 "$(grep -c Botan exports)"
    assertEquals "Unexpected: 'OpenSSL' is in exports" 0 "$(grep -c OpenSSL exports)"
    assertEquals "Unexpected: 'rnp_version_string_full' is not in exports" 1 "$(grep -c rnp_version_string_full exports)"

    rm -f exports
}

test_supported_features() {
    # Make sure that we support all features which should be supported
    supported=( RSA ELGAMAL DSA ECDH ECDSA EDDSA \
        TRIPLEDES CAST5 BLOWFISH AES128 AES192 AES256 CAMELLIA128 CAMELLIA192 CAMELLIA256 \
        MD5 SHA1 RIPEMD160 SHA256 SHA384 SHA512 SHA224 SHA3-256 SHA3-512 \
        ZIP ZLIB BZip2 \
        "NIST P-256" "NIST P-384" "NIST P-521" Ed25519 Curve25519 secp256k1 \
        OCB)

    # Old versions say ${unsupported[@]} is unbound if empty
    unsupported=( NOOP )

    # Features to ignore in testing
    ignored=( NOOP )

    botan_only=( TWOFISH EAX )
    brainpool=( brainpoolP256r1 brainpoolP384r1 brainpoolP512r1 )
    sm2=( SM2 SM4 SM3 "SM2 P-256" )

    # SM2
    if [[ "$ENABLE_SM2" == "Off" ]]; then
        unsupported+=("${sm2[@]}")
    elif [[ "${CRYPTO_BACKEND:-}" == "openssl" ]]; then
        unsupported+=("${sm2[@]}")
    else
        supported+=("${sm2[@]}")
    fi

    # IDEA
    if [[ "$ENABLE_IDEA" == "Off" ]]; then
        unsupported+=(IDEA)
    else
        supported+=(IDEA)
    fi

    case "$OSTYPE" in
      msys|cygwin)
        so_folder="bin"
        support+=("${brainpool[@]}")
        # OpenSSL on msys doesn't seem to have legacy provider anymore
        if [[ "${CRYPTO_BACKEND:-}" == "openssl" ]]; then
            ignored+=( CAST5 BLOWFISH IDEA )
        fi
        ;;
      darwin*)
        so_folder="lib"
        support+=("${brainpool[@]}")
        ;;
      *)
        so_folder="lib64"
#       botan_only+=("${brainpool[@]}")
    esac

    if [[ "${CRYPTO_BACKEND:-}" == "openssl" ]]; then
        unsupported+=("${botan_only[@]}")
        library_path="${JSONC_INSTALL}/$so_folder:${RNP_INSTALL}/$so_folder"
    else
        supported+=("${botan_only[@]}")
        library_path="${BOTAN_INSTALL}/$so_folder:${JSONC_INSTALL}/$so_folder:${RNP_INSTALL}/$so_folder"
    fi

    # For darwin we assume that LC_RPATH is added with @executable_dir/../lib
    if [[ ! "$OSTYPE" == darwin* ]]; then
        export LD_LIBRARY_PATH="$library_path${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    fi

    "$RNP_INSTALL"/bin/rnp --version > rnp-version

    for feature in "${supported[@]}"
    do
        if [[ "${ignored[*]}" == *${feature}* ]]; then
            continue
        fi
        fea="$(grep -ci "$feature" rnp-version)"
        assertTrue "Unexpected unsupported feature: '$feature'" "[[ $fea -ge 1 ]]"
    done
    for feature in "${unsupported[@]}"
    do
        if [[ "${ignored[*]}" == *${feature}* ]]; then
            continue
        fi
        fea="$(grep -ci "$feature" rnp-version)"
        assertTrue "Unexpected supported feature: '$feature'" "[[ $fea == 0 ]]"
    done

    rm -f rnp-version
}

# ......................................................................
# shellcheck source=/dev/null
. "$DIR0"/shunit2/shunit2
