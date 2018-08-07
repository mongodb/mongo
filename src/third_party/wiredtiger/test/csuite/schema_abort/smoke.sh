#! /bin/sh

set -e

# Smoke-test schema-abort as part of running "make check".

$TEST_WRAPPER ./test_schema_abort -t 10 -T 5
$TEST_WRAPPER ./test_schema_abort -m -t 10 -T 5
$TEST_WRAPPER ./test_schema_abort -C -t 10 -T 5
$TEST_WRAPPER ./test_schema_abort -C -m -t 10 -T 5
$TEST_WRAPPER ./test_schema_abort -m -t 10 -T 5 -z
$TEST_WRAPPER ./test_schema_abort -m -t 10 -T 5 -x
