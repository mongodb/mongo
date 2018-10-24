#! /bin/sh

set -e

# Smoke-test looking for checkpoint races with and without data as part of
# running "make check".

$TEST_WRAPPER ./test_wt3363_checkpoint_op_races
$TEST_WRAPPER ./test_wt3363_checkpoint_op_races -d
