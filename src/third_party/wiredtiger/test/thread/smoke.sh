#!/bin/sh

set -e

# Smoke-test format as part of running "make check".
$TEST_WRAPPER ./t -t f
$TEST_WRAPPER ./t -S -F -t f

$TEST_WRAPPER ./t -t r
$TEST_WRAPPER ./t -S -F -t r

$TEST_WRAPPER ./t -t v
$TEST_WRAPPER ./t -S -F -t v
