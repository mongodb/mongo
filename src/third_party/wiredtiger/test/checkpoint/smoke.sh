#! /bin/sh

set -e

# Bypass this test for valgrind
test "$TESTUTIL_BYPASS_VALGRIND" = "1" && exit 0

# Smoke-test checkpoints as part of running "make check".
echo "checkpoint: 3 mixed tables"
$TEST_WRAPPER ./t -T 3 -t m

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

echo "checkpoint: row-store tables, stress LAS. Sweep and timestamps"
$TEST_WRAPPER ./t -t r -W 3 -r 2 -s -x -n 100000 -k 100000 -C cache_size=100MB

echo "checkpoint: 3 mixed tables, with sweep"
$TEST_WRAPPER ./t -T 3 -t m -W 3 -r 2 -s -n 100000 -k 100000

echo "checkpoint: 3 mixed tables, with timestamps"
$TEST_WRAPPER ./t -T 3 -t m -W 3 -r 2 -x -n 100000 -k 100000
