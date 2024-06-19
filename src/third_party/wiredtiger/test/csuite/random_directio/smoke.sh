#! /bin/sh

set -e

# Smoke-test random_directio as part of running "make check".

# If $binary_dir isn't set, default to using the build directory
# this script resides under. Our CMake build will sync a copy of this
# script to the build directory. Note this assumes we are executing a
# copy of the script that lives under the build directory. Otherwise
# passing the binary path is required.
binary_dir=${binary_dir:-`dirname $0`}

if [ -n "$1" ]
then
    RUN_TEST_CMD="$TEST_WRAPPER $1"
else
    RUN_TEST_CMD="$TEST_WRAPPER $binary_dir/test_random_directio"
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
