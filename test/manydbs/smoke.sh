#!/bin/sh

trap 'chmod -R u+w WT_*; exit 0' 0 1 2 3 13 15

set -e

# Smoke-test format as part of running "make check".
# Run with:
# 1.  The defaults
# 2.  Set idle flag to turn off operations.
# 3.  More dbs.
# 
echo "manydbs: default with operations turned on"
$TEST_WRAPPER ./t
echo "manydbs: totally idle databases"
$TEST_WRAPPER ./t -I
echo "manydbs: 40 databases with operations"
$TEST_WRAPPER ./t -D 40
echo "manydbs: 40 idle databases"
$TEST_WRAPPER ./t -I -D 40
