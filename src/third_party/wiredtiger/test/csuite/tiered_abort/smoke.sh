#! /bin/sh

set -e

# Return success from the script because the test itself does not yet work.
# Do this in the script so that we can manually run the program on the command line.
exit 0
# Smoke-test tiered-abort as part of running "make check".

if [ -n "$1" ]
then
    # If the test binary is passed in manually.
    test_bin=$1
else
    # If $top_builddir/$top_srcdir aren't set, default to building in build_posix
    # and running in test/csuite.
    top_builddir=${top_builddir:-../../build_posix}
    top_srcdir=${top_srcdir:-../..}
    test_bin=$top_builddir/test/csuite/test_tiered_abort
fi
$TEST_WRAPPER $test_bin -t 10 -T 5
