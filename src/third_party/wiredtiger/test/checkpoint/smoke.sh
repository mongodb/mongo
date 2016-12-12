#! /bin/sh

set -e

# Smoke-test checkpoints as part of running "make check".
echo "checkpoint: 3 mixed tables"
$TEST_WRAPPER ./t -T 3 -t m

# We are done if short tests are requested
test -z "$TESTUTIL_DISABLE_LONG_TESTS" || exit 0

echo "checkpoint: 6 column-store tables"
$TEST_WRAPPER ./t -T 6 -t c

echo "checkpoint: 6 LSM tables"
$TEST_WRAPPER ./t -T 6 -t l

echo "checkpoint: 6 mixed tables"
$TEST_WRAPPER ./t -T 6 -t m

echo "checkpoint: 6 row-store tables"
$TEST_WRAPPER ./t -T 6 -t r

echo "checkpoint: 6 row-store tables, named checkpoint"
$TEST_WRAPPER ./t -c 'TeSt' -T 6 -t r
