#! /bin/sh

# Smoke-test checkpoints as part of running "make check".

echo "checkpoint: 3 mixed tables"
$TEST_WRAPPER ./t -T 3 -t m || exit 1

echo "checkpoint: 6 column-store tables"
$TEST_WRAPPER ./t -T 6 -t c || exit 1

echo "checkpoint: 6 LSM tables"
$TEST_WRAPPER ./t -T 6 -t l || exit 1

echo "checkpoint: 6 mixed tables"
$TEST_WRAPPER ./t -T 6 -t m || exit 1

echo "checkpoint: 6 row-store tables"
$TEST_WRAPPER ./t -T 6 -t r || exit 1

echo "checkpoint: 6 row-store tables, named checkpoint"
$TEST_WRAPPER ./t -c 'TeSt' -T 6 -t r || exit 1
