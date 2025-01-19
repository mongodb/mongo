#!/bin/bash
# This script needs to be called from the build directory.
# It runs a variety of live restore configurations in sequence.
source "../test/live_restore/helper.sh"

# 30 iterations with 20000 operations per iteration and 20 collections.
run_test "-i 30 -l 2 -o 20000 -c 20"

# Background thread debug mode test, this will wait for the background thread to complete prior to
# exiting, but it will not apply crud operations after startup.
run_test "-i 2 -l 2 -b -t 1"

# 5 iterations with 200K operations dying at some point. Followed by an single 200K operation
# iteration after performing recovery
run_test "-i 5 -l 2 -o 200000 -t 12 -d"
run_test "-i 1 -l 2 -r -o 200000 -t 12"
