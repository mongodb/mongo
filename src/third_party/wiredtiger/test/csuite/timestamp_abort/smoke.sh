#! /bin/sh

set -e

# Smoke-test timestamp-abort as part of running "make check".

# If $top_builddir/$top_srcdir aren't set, default to building in build_posix
# and running in test/csuite.
top_builddir=${top_builddir:-../../build_posix}
top_srcdir=${top_srcdir:-../..}

 $TEST_WRAPPER $top_builddir/test/csuite/test_timestamp_abort -t 10 -T 5
#$TEST_WRAPPER $top_builddir/test/csuite/test_timestamp_abort -t 10 -T 5 -L
 $TEST_WRAPPER $top_builddir/test/csuite/test_timestamp_abort -m -t 10 -T 5
#$TEST_WRAPPER $top_builddir/test/csuite/test_timestamp_abort -m -t 10 -T 5 -L
 $TEST_WRAPPER $top_builddir/test/csuite/test_timestamp_abort -C -t 10 -T 5
 $TEST_WRAPPER $top_builddir/test/csuite/test_timestamp_abort -C -m -t 10 -T 5
