#!/bin/sh

trap 'chmod -R u+w WT_*' 0 1 2 3 13 15

set -e

# Smoke-test format as part of running "make check".
$TEST_WRAPPER ./t
