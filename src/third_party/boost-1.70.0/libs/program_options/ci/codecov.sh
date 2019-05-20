#! /bin/bash
#
# Copyright 2017, 2018 James E. King III
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
#      http://www.boost.org/LICENSE_1_0.txt)
#
# Bash script to run in travis to perform codecov.io integration
#

###
### NOTE: Make sure you grab .codecov.yml
###

# assumes cwd is the top level directory of the boost project
# assumes an environment variable $SELF is the boost project name

set -ex

B2_VARIANT=debug
ci/build.sh cxxflags=-fprofile-arcs cxxflags=-ftest-coverage linkflags=-fprofile-arcs linkflags=-ftest-coverage

# switch back to the original source code directory
cd $TRAVIS_BUILD_DIR

# get the version of lcov
lcov --version

# coverage files are in ../../b2 from this location
lcov --gcov-tool=gcov-7 --rc lcov_branch_coverage=1 --base-directory "$BOOST_ROOT/libs/$SELF" --directory "$BOOST_ROOT" --capture --output-file all.info

# all.info contains all the coverage info for all projects - limit to ours
lcov --gcov-tool=gcov-7 --rc lcov_branch_coverage=1 --extract all.info "*/boost/$SELF/*" "*/libs/$SELF/src/*" --output-file coverage.info

# dump a summary on the console - helps us identify problems in pathing
lcov --gcov-tool=gcov-7 --rc lcov_branch_coverage=1 --list coverage.info

#
# upload to codecov.io
#
curl -s https://codecov.io/bash > .codecov
chmod +x .codecov
./.codecov -f coverage.info -X gcov -x "gcov-7"
