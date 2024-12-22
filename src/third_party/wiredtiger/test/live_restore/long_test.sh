#!/bin/bash
# This script needs to be called from the build directory.
# It runs a variety of live restore configurations in sequence.
source "../test/live_restore/helper.sh"

# 20 iterations with 20000 operations per iteration.
run_test "-i 20 -l 2 -o 20000"

# Background thread debug mode test, this will wait for the background thread to complete prior to
# exiting, but it will not apply crud operations after startup.
run_test "-i 2 -l 2 -b"

# 3 iterations with 200K operations"
run_test "-i 3 -l 2 -o 200000"
