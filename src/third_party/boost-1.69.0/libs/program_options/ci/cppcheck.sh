#! /bin/bash
#
# Copyright 2018 James E. King III
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
#      http://www.boost.org/LICENSE_1_0.txt)
#
# Bash script to run in travis to perform a cppcheck
# cwd should be $BOOST_ROOT before running
#

set -ex

# default language level: c++03
if [[ -z "$CXXSTD" ]]; then
    CXXSTD=03
fi

# Travis' ubuntu-trusty comes with cppcheck 1.62 which is pretty old
# default cppcheck version: 1.82
if [[ -z "$CPPCHKVER" ]]; then
    CPPCHKVER=1.82
fi

pushd ~
wget https://github.com/danmar/cppcheck/archive/$CPPCHKVER.tar.gz
tar xzf $CPPCHKVER.tar.gz
mkdir cppcheck-build
cd cppcheck-build
cmake ../cppcheck-$CPPCHKVER -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_INSTALL_PREFIX=~/cppcheck
make -j3 install
popd

~/cppcheck/bin/cppcheck -I. --std=c++$CXXSTD --enable=all --error-exitcode=1 \
         --force --check-config --suppress=*:boost/preprocessor/tuple/size.hpp \
         -UBOOST_USER_CONFIG -UBOOST_COMPILER_CONFIG -UBOOST_STDLIB_CONFIG -UBOOST_PLATFORM_CONFIG \
         libs/$SELF 2>&1 | grep -v 'Cppcheck does not need standard library headers'

