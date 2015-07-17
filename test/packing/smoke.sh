#! /bin/sh

set -e

# Smoke-test integer packing as part of running "make check".  Don't just run
# everything because some of the code in this directory is aimed at testing the
# performance of the packing code

$TEST_WRAPPER ./packing-test
$TEST_WRAPPER ./intpack-test3
