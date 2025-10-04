#!/bin/bash
# This script needs to be called from the build directory.
# It runs a variety of live restore configurations in sequence.
source "../test/live_restore/helper.sh"

# 10 iterations with 20k operations per iteration and 20 collections.
run_test "-i 10 -l 2 -o 20000 -c 20"

# 10 iterations with 20k operations per iteration and 20 collections for per directory db usage.
run_test "-i 10 -l 2 -o 20000 -c 20 -D"

# Background thread debug mode test, this will wait for the background thread to complete prior to
# exiting, but it will not apply crud operations after startup.
run_test "-i 2 -l 2 -b -t 1"

# 5 iterations with 50K operations dying at some point. Followed by an single 50K operation
# iteration after performing recovery.
run_test "-i 5 -l 2 -o 50000 -t 12 -d"
run_test "-i 1 -l 2 -r -o 50000 -t 12"

# 5 iterations with 50K operations dying at some point. Followed by an single 50K operation
# iteration after performing recovery for per directory db usage.
run_test "-i 5 -l 2 -o 50000 -t 12 -d -D"
run_test "-i 1 -l 2 -r -o 50000 -t 12 -D"
