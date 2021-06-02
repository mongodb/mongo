#! /bin/sh

set -e

# Smoke-test incr-backup as part of running "make check".

if [ -n "$1" ]
then
    # If the test binary is passed in manually.
    $TEST_WRAPPER $1 -v 3
else
    # If $top_builddir/$top_srcdir aren't set, default to building in build_posix
    # and running in test/csuite.
    top_builddir=${top_builddir:-../../build_posix}
    top_srcdir=${top_srcdir:-../..}

    $TEST_WRAPPER $top_builddir/test/csuite/test_incr_backup -v 3
fi
