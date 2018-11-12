#! /bin/bash
#
# Copyright 2017 James E. King III
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
#      http://www.boost.org/LICENSE_1_0.txt)
#
# Bash script to run in travis to perform a bjam build
# cwd should be $BOOST_ROOT/libs/$SELF before running
#

set -ex

# default language level: c++03
if [[ -z "$CXXSTD" ]]; then
    CXXSTD=03
fi

$BOOST_ROOT/b2 . toolset=$TOOLSET cxxstd=$CXXSTD $CXXFLAGS $DEFINES $LINKFLAGS $TESTFLAGS $B2_ADDRESS_MODEL $B2_LINK $B2_THREADING $B2_VARIANT -j3 $*
