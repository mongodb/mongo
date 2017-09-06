#! /bin/sh

set -e

# Smoke-test timestamp-abort as part of running "make check".

$TEST_WRAPPER ./test_timestamp_abort -t 10 -T 5
$TEST_WRAPPER ./test_timestamp_abort -m -t 10 -T 5
$TEST_WRAPPER ./test_timestamp_abort -C -t 10 -T 5
$TEST_WRAPPER ./test_timestamp_abort -C -m -t 10 -T 5
