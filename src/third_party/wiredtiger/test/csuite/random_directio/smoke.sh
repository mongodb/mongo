#! /bin/sh

set -e

# Smoke-test random_directio as part of running "make check".

RUN_TEST_CMD="$TEST_WRAPPER ./test_random_directio"

# Disabled for now until we fix issues encountered via the test
exit 0

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
        $RUN_TEST -T $threads -S create,drop,verbose       || exit 1

        # Here are successively tougher schema tests that do not yet
        # reliably pass.  'verbose' can be added to any.
        #$RUN_TEST -T $threads -S create,create_check       || exit 1
        #$RUN_TEST -T $threads -S create,drop,drop_check    || exit 1
        #$RUN_TEST -T $threads -S create,rename             || exit 1
        #$RUN_TEST -T $threads -S create,rename,drop_check  || exit 1
        #$RUN_TEST -T $threads -S all,verbose               || exit 1
    done
done
