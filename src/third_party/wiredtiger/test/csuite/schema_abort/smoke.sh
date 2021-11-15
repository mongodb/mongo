#! /bin/sh

set -e

# Smoke-test schema-abort as part of running "make check".


if [ -n "$1" ]
then
    # If the test binary is passed in manually.
    test_bin=$1
else
    # If $top_builddir/$top_srcdir aren't set, default to building in build_posix
    # and running in test/csuite.
    top_builddir=${top_builddir:-../../build_posix}
    top_srcdir=${top_srcdir:-../..}
    test_bin=$top_builddir/test/csuite/test_schema_abort
fi

$TEST_WRAPPER $test_bin -t 10 -T 5
$TEST_WRAPPER $test_bin -m -t 10 -T 5
$TEST_WRAPPER $test_bin -C -t 10 -T 5
$TEST_WRAPPER $test_bin -C -m -t 10 -T 5

$TEST_WRAPPER $test_bin -c -t 10 -T 5
$TEST_WRAPPER $test_bin -c -m -t 10 -T 5
$TEST_WRAPPER $test_bin -c -C -t 10 -T 5
$TEST_WRAPPER $test_bin -c -C -m -t 10 -T 5

$TEST_WRAPPER $test_bin -m -t 10 -T 5 -z
$TEST_WRAPPER $test_bin -c -m -t 10 -T 5 -z
$TEST_WRAPPER $test_bin -m -t 10 -T 5 -x
$TEST_WRAPPER $test_bin -c -m -t 10 -T 5 -x
