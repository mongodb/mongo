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
DIR_LIB="$INSTALL_PREFIX/lib/x86_64-linux-gnu"
DIR_INC="$INSTALL_PREFIX/include/rnp"
DIR_BIN="$INSTALL_PREFIX/bin"
#DIR_MAN="$INSTALL_PREFIX/share/man"
DIR_CMAKE="$INSTALL_PREFIX/lib/x86_64-linux-gnu/cmake/rnp"

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

# Man page installation does not work as expected
#declare expected_manuals=(
#    "$DIR_MAN/man3/librnp.3.gz"
#    "$DIR_MAN/man1/rnp.1.gz"
#    "$DIR_MAN/man1/rnpkeys.1.gz"
#)

t_installed_files() {
    local f=
    for f in "$@"
    do
        assertTrue "$f was not installed" "[ -e $f ]"
    done
}

test_installed_files_librnp() {
# shellcheck disable=SC2046
    sudo dpkg -i $(ls ./*.deb) || sudo apt-get -y -f install

    t_installed_files "${expected_libraries[@]}"
    t_installed_files "${expected_devlibraries[@]}"
    t_installed_files "${expected_includes[@]}"
    t_installed_files "${expected_cmakefiles[@]}"
    t_installed_files "${expected_binaries[@]}"

# Man page installation does not work as expected
#    t_installed_files "${expected_manuals[@]}"

    sudo dpkg -r rnp0
}

# ......................................................................
# shellcheck source=/dev/null
. "$DIR0"/shunit2/shunit2
