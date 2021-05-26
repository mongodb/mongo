#! /bin/sh

set -e

# Smoke-test random_directio as part of running "make check".

# If $top_builddir/$top_srcdir aren't set, default to building in build_posix
# and running in test/csuite.
top_builddir=${top_builddir:-../../build_posix}
top_srcdir=${top_srcdir:-../..}

if [ -n "$1" ]
then
    RUN_TEST_CMD="$TEST_WRAPPER $1"
else
    RUN_TEST_CMD="$TEST_WRAPPER $top_builddir/test/csuite/test_random_directio"
fi
# Replace for more complete testing
#TEST_THREADS="1 5 10"
TEST_THREADS="5"

# Replace for more complete testing
#TEST_METHODS="none dsync fsync"
TEST_METHODS="none"

for threads in $TEST_THREADS; do
    for method in $TEST_METHODS; do
        RUN_TEST="$RUN_TEST_CMD -t 5 -m $method"
        $RUN_TEST -T $threads                              || exit 1
        $RUN_TEST -f 20 -T $threads -S create,drop,verbose       || exit 1

        # Here are successively tougher schema tests that do not yet
        # reliably pass.  'verbose' can be added to any.
        #$RUN_TEST -T $threads -S create,integrated,create_check      || exit 1
        #$RUN_TEST -T $threads -S create,integrated,drop,drop_check   || exit 1
        #$RUN_TEST -T $threads -S create,integrated,rename            || exit 1
        #$RUN_TEST -T $threads -S create,integrated,rename,drop_check || exit 1
        #$RUN_TEST -T $threads -S all,verbose                         || exit 1
    done
done
