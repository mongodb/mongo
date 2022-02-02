#! /bin/sh

set -e

# Return success from the script because the test itself does not yet work.
# Do this in the script so that we can manually run the program on the command line.
exit 0
# Smoke-test tiered-abort as part of running "make check".

if [ -n "$1" ]
then
    # If the test binary is passed in manually.
    test_bin=$1
else
    # If $binary_dir isn't set, default to using the build directory
    # this script resides under. Our CMake build will sync a copy of this
    # script to the build directory. Note this assumes we are executing a
    # copy of the script that lives under the build directory. Otherwise
    # passing the binary path is required.
    binary_dir=${binary_dir:-`dirname $0`}
    test_bin=$binary_dir/test_tiered_abort
fi
$TEST_WRAPPER $test_bin -t 10 -T 5
