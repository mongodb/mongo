#! /bin/bash
#
# Copyright (c) 2023-2025 [Ribose Inc](https://www.ribose.com).
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

DIR_CMAKE="$INSTALL_PREFIX/lib64/cmake/rnp"

create_source_file() {
    cat <<"EOF" > find_package_test.cpp
        #include <rnp/rnp.h>
        #include <rnp/rnp_ver.h>

        int main(int argc, char *argv[]) {
            printf("RNP version: %s\n", rnp_version_string());
            printf("RNP backend: %s\n", RNP_BACKEND);
            printf("RNP backend version: %s\n", RNP_BACKEND_VERSION);
            printf("RNP has AEAD: %d\n", RNP_HAS_AEAD);
            return 0;
        }
EOF
}

create_cmake_file() {
    cat <<"EOF" > CMakeLists.txt
        project(find_package_test)

        find_package(PkgConfig REQUIRED)

        find_package(BZip2 REQUIRED)
        find_package(ZLIB REQUIRED)

        pkg_check_modules(JSONC IMPORTED_TARGET json-c12)
        if(NOT JSONC_FOUND)
            pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
        endif(NOT JSONC_FOUND)

        add_library(JSON-C::JSON-C INTERFACE IMPORTED)
        set_target_properties(JSON-C::JSON-C PROPERTIES INTERFACE_LINK_LIBRARIES PkgConfig::JSONC)

        pkg_check_modules(Botan REQUIRED IMPORTED_TARGET botan-2)
        add_library(Botan::Botan  INTERFACE IMPORTED)
        set_target_properties(Botan::Botan PROPERTIES INTERFACE_LINK_LIBRARIES PkgConfig::Botan)

        find_package(rnp REQUIRED)

        cmake_minimum_required(VERSION 3.12)
        add_executable(find_package_test find_package_test.cpp)
EOF
    echo  "target_link_libraries(find_package_test $1)">>CMakeLists.txt
}

test_shared_library() {
    sudo yum -y localinstall librnp0-0*.*.rpm librnp0-devel-0*.*.rpm
    pushd "$(mktemp -d)"
    create_source_file
    create_cmake_file 'rnp::librnp'

# shellcheck disable=SC2251
!   cmake . -DCMAKE_MODULE_PATH="$DIR_CMAKE"/*
    assertEquals "cmake failed at shared library test" 0 "${PIPESTATUS[0]}"

# shellcheck disable=SC2251
!   make
    assertEquals "make failed at shared library test" 0 "${PIPESTATUS[0]}"

# shellcheck disable=SC2251
!   ./find_package_test
    assertEquals "test program failed at shared library test" 0 "${PIPESTATUS[0]}"

# shellcheck disable=SC2251
!   ldd find_package_test | grep librnp
    assertEquals "no reference to shared rnp library at shared library test" 0 "${PIPESTATUS[1]}"

    popd
# shellcheck disable=SC2046
    sudo yum -y erase $(rpm -qa  | grep rnp)
}

test_no_library() {
    pushd "$(mktemp -d)"
    create_source_file
    create_cmake_file 'rnp::librnp'

# shellcheck disable=SC2251
!   cmake . -DCMAKE_MODULE_PATH="$DIR_CMAKE"/*
    assertNotEquals "cmake succeeded at no library test" 0 "${PIPESTATUS[0]}"
    popd
}

# ......................................................................
# shellcheck source=/dev/null
. "$DIR0"/shunit2/shunit2
