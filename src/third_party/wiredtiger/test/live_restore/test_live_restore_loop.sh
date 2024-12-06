#!/bin/bash

# Run the cppsuite live_restore test multiple times. The first run creates a a database, and
# then subsequent runs use the previous run's database as the source directory.
# This script must be run from the root of the build directory.

rm -f live_restore_loop.log

test -d CMakeFiles || { echo "Please run this script from the root of the build directory."; exit 1; }

# Run the setup.
echo "Starting a fresh live restore run!"
./test/cppsuite/test_live_restore -f > live_restore_loop.log 2>&1
exit=0
it_count=1
# Loop-de-loop.
while [ $exit -eq 0 ]
do
    echo "Executing subsequent run:" $it_count
    # This will continuously grow the log. Switch this back to > once less bugs occur.
    ./test/cppsuite/test_live_restore >> live_restore_loop.log 2>&1
    exit=$?
    # If a count is supplied and we've reached it, exit.
    if [ ! -z "$1" ] && [ $it_count -eq $1 ]; then
        break
    fi

    ((it_count++))
done

if [ $exit -ne 0 ]; then
    echo "==================="
    echo "Live restore loop failed! Exit code: " $exit " Logs located in live_restore_loop.log"
    echo "==================="
    exit $exit
else
    echo "Script finished, no errors encountered!"
fi
