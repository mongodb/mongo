#! /bin/sh

set -e

# Smoke-test recovery as part of running "make check".

$TEST_WRAPPER ./random-abort -t 10 -T 5
$TEST_WRAPPER ./random-abort -m -t 10 -T 5
$TEST_WRAPPER ./truncated-log
