#! /bin/sh

set -e

# Smoke-test random-abort as part of running "make check".

$TEST_WRAPPER ./test_random_abort -t 10 -T 5
$TEST_WRAPPER ./test_random_abort -m -t 10 -T 5
$TEST_WRAPPER ./test_random_abort -C -t 10 -T 5
$TEST_WRAPPER ./test_random_abort -C -m -t 10 -T 5
