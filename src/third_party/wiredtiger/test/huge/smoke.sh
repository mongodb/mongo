#! /bin/sh

set -e

# Smoke-test as part of running "make check".
$TEST_WRAPPER ./t -s
