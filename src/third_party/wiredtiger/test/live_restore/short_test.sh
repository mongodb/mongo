#!/bin/bash
# This script needs to be called from the build directory.
# It runs a variety of live restore configurations in sequence.
source ../test/live_restore/helper.sh
# 10 iterations, 1 collection, 1000 operations, log level INFO
run_test "-i 10 -l 2 -c 1 -o 1000"

# Die, then run recovery.
run_test "-i 2 -l 2 -d -o 10"
run_test "-i 1 -l 2 -r -o 10"
