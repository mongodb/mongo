#! /bin/sh

set -e

# Smoke-test random-abort as part of running "make check".

if [ -n "$1" ]
then
    # If the test binary is passed in manually.
    test_bin=$1
else
    # If $top_builddir/$top_srcdir aren't set, default to building in build_posix
    # and running in test/csuite.
    top_builddir=${top_builddir:-../../build_posix}
    top_srcdir=${top_srcdir:-../..}
    test_bin=$top_builddir/test/csuite/test_random_abort
fi
$TEST_WRAPPER $test_bin -t 10 -T 5
$TEST_WRAPPER $test_bin -m -t 10 -T 5
$TEST_WRAPPER $test_bin -C -t 10 -T 5
$TEST_WRAPPER $test_bin -C -m -t 10 -T 5
