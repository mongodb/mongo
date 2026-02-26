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

# Defaults applicable to 'normal' installation and not build environment
: "${INSTALL_PREFIX:=/usr}"
: "${BOTAN_INSTALL:=$INSTALL_PREFIX}"
: "${JSONC_INSTALL:=$INSTALL_PREFIX}"
: "${RNP_INSTALL:=$INSTALL_PREFIX}"

: "${ENABLE_SM2:=}"
: "${ENABLE_IDEA:=}"

DIR_LIB="$INSTALL_PREFIX/lib64"
DIR_INC="$INSTALL_PREFIX/include/rnp"
DIR_BIN="$INSTALL_PREFIX/bin"
DIR_MAN="$INSTALL_PREFIX/share/man"
DIR_CMAKE="$INSTALL_PREFIX/lib64/cmake/rnp"

declare expected_libraries=(
    "$DIR_LIB/librnp.so.0"
)

declare expected_devlibraries=(
    "$DIR_LIB/librnp.so"
    "$DIR_LIB/pkgconfig/librnp.pc"
)

declare expected_includes=(
    "$DIR_INC/rnp.h"
    "$DIR_INC/rnp_err.h"
    "$DIR_INC/rnp_export.h"
)

declare expected_cmakefiles=(
    "$DIR_CMAKE/rnp-config.cmake"
    "$DIR_CMAKE/rnp-config-version.cmake"
    "$DIR_CMAKE/rnp-targets.cmake"
    "$DIR_CMAKE/rnp-targets-release.cmake"
)

declare expected_binaries=(
    "$DIR_BIN/rnp"
    "$DIR_BIN/rnpkeys"
)

declare expected_manuals=(
    "$DIR_MAN/man3/librnp.3.gz"
    "$DIR_MAN/man1/rnp.1.gz"
    "$DIR_MAN/man1/rnpkeys.1.gz"
)

test_installed_files() {
    local f=
    for f in "$@"
    do
        assertTrue "$f was not installed" "[ -e $f ]"
    done
}

test_installed_files_librnp() {
    sudo yum -y localinstall librnp0-0*.*.rpm
    test_installed_files "${expected_libraries[@]}"
# shellcheck disable=SC2046
    sudo yum -y erase $(rpm -qa  | grep rnp)
}

test_installed_files_librnp-devel() {
    sudo yum -y localinstall librnp0-0*.*.rpm librnp0-devel-0*.*.rpm
    test_installed_files "${expected_libraries[@]}"
    test_installed_files "${expected_devlibraries[@]}"
    test_installed_files "${expected_includes[@]}"
    test_installed_files "${expected_cmakefiles[@]}"
# shellcheck disable=SC2046
    sudo yum -y erase $(rpm -qa  | grep rnp)
}

test_installed_files_rnp() {
    sudo yum -y localinstall librnp0-0*.*.rpm rnp0-0*.*.rpm
    test_installed_files "${expected_libraries[@]}"
    test_installed_files "${expected_binaries[@]}"
# shellcheck disable=SC2046
    sudo yum -y erase $(rpm -qa  | grep rnp)
}

test_installed_files_doc() {
# in case the nodocs transaction flag is set in the yum configuration
    sudo yum --setopt=tsflags='' -y install man-db
    sudo yum --setopt=tsflags='' -y localinstall rnp-*-doc.rpm
    test_installed_files "${expected_manuals[@]}"
# shellcheck disable=SC2046
    sudo yum -y erase $(rpm -qa  | grep rnp)
}

# ......................................................................
# shellcheck source=/dev/null
. "$DIR0"/shunit2/shunit2
