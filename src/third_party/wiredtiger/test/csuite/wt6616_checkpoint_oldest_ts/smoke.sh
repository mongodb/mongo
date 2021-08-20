#! /bin/sh

set -e

# Smoke-test wt6616_checkpoint_oldest_ts as part of running "make check".

if [ -n "$1" ]
then
    # If the test binary is passed in manually.
    test_bin=$1
else
    # If $top_builddir/$top_srcdir aren't set, default to building in build_posix
    # and running in test/csuite.
    top_builddir=${top_builddir:-../../build_posix}
    top_srcdir=${top_srcdir:-../..}
    test_bin=$top_builddir/test/csuite/test_wt6616_checkpoint_oldest_ts
fi

$TEST_WRAPPER $test_bin 
$TEST_WRAPPER $test_bin -c

