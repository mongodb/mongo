#! /bin/sh

set -e

# Smoke-test schema-disagg-abort as part of running "make check".

if [ -n "$1" ]
then
    test_bin=$1
else
    binary_dir=${binary_dir:-`dirname $0`}
    test_bin=$binary_dir/test_schema_disagg_abort
fi

# Resolve the build directory (two levels up from the test binary).
build_dir=$(cd "$(dirname "$test_bin")/../../../" && pwd)

$TEST_WRAPPER "$test_bin" -b "$build_dir" -t 10 -T 2 -h WT_TEST.schema_disagg_abort.t2
$TEST_WRAPPER "$test_bin" -b "$build_dir" -t 10 -T 4 -h WT_TEST.schema_disagg_abort.t4
$TEST_WRAPPER "$test_bin" -b "$build_dir" -t 10 -T 2 -s 4 -h WT_TEST.schema_disagg_abort.s4
$TEST_WRAPPER "$test_bin" -b "$build_dir" -t 10 -T 2 -s 16 -h WT_TEST.schema_disagg_abort.s16
