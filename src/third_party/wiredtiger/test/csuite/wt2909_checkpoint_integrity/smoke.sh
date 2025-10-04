#! /bin/sh

set -e

# Smoke-test wt2909_checkpoint_integrity as part of running "make check".

# The cmake build passes in the builddir with -b; it should be passed through.
builddir_arg=
while getopts ":b:" opt; do
    case $opt in
        b) builddir_arg="-b $OPTARG" ;;
    esac
done
shift $(( OPTIND - 1 ))

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
    test_bin=$binary_dir/test_wt2909_checkpoint_integrity
fi

$TEST_WRAPPER $test_bin $builddir_arg -t r
$TEST_WRAPPER $test_bin $builddir_arg -t c
