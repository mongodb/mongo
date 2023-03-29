#! /bin/sh

set -e

# Smoke-test timestamp-abort as part of running "make check". Use the -s option
# to add a stress timing in checkpoint prepare.

default_test_args="-t 10 -T 5"
while getopts ":sb:" opt; do
    case $opt in
        s) default_test_args="$default_test_args -s" ;;
        b) test_bin=$OPTARG ;;
    esac
done

if [ -z "$test_bin" ]
then
    # If $binary_dir isn't set, default to using the build directory
    # this script resides under. Our CMake build will sync a copy of this
    # script to the build directory. Note this assumes we are executing a
    # copy of the script that lives under the build directory. Otherwise
    # passing the binary path is required.
    binary_dir=${binary_dir:-`dirname $0`}
    test_bin=$binary_dir/test_timestamp_abort
fi

$TEST_WRAPPER $test_bin $default_test_args
$TEST_WRAPPER $test_bin $default_test_args -c
#$TEST_WRAPPER $test_bin $default_test_args -L
#$TEST_WRAPPER $test_bin $default_test_args -B -I 3
$TEST_WRAPPER $test_bin -m $default_test_args
$TEST_WRAPPER $test_bin -m $default_test_args -c
#$TEST_WRAPPER $test_bin -m $default_test_args -L
$TEST_WRAPPER $test_bin -C $default_test_args
$TEST_WRAPPER $test_bin -C $default_test_args -c
#$TEST_WRAPPER $test_bin -C $default_test_args -B -I 3
$TEST_WRAPPER $test_bin -C -m $default_test_args
$TEST_WRAPPER $test_bin -C -m $default_test_args -c
