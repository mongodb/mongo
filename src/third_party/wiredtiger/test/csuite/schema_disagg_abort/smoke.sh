#! /bin/sh

set -e

# Smoke-test schema-disagg-abort as part of running "make check".

# The build directory holds the PALite extension the test loads. The cmake build can pass it in
# with -b; otherwise it is derived from where the binary sits in the build tree.
build_dir=
while getopts ":b:" opt; do
    case $opt in
        b) build_dir=$OPTARG ;;
    esac
done
shift $(( OPTIND - 1 ))

if [ -n "$1" ]
then
    # If the test binary is passed in manually.
    test_bin=$1
else
    # If $binary_dir isn't set, default to using the build directory this script resides under.
    # Our CMake build syncs a copy of this script next to the binary, so running that copy works;
    # the copy in the source tree has no binary beside it and needs the path passed in.
    binary_dir=${binary_dir:-`dirname $0`}
    test_bin=$binary_dir/test_schema_disagg_abort
fi

if [ ! -x "$test_bin" ]
then
    echo "$0: no test binary at \"$test_bin\"." >&2
    echo "Run the copy of this script that cmake put in the build directory, or name the binary:" >&2
    echo "    sh $0 <build>/test/csuite/schema_disagg_abort/test_schema_disagg_abort" >&2
    exit 1
fi

# Three levels up from the binary in the build tree.
[ -n "$build_dir" ] || build_dir=$(cd "$(dirname "$test_bin")/../../.." && pwd)

# Single-node graceful runs.
$TEST_WRAPPER "$test_bin" -b "$build_dir" -r l -t 10 -T 2 -h WT_TEST.schema_disagg_abort.l
$TEST_WRAPPER "$test_bin" -b "$build_dir" -r l -t 10 -T 4 -h WT_TEST.schema_disagg_abort.l4
$TEST_WRAPPER "$test_bin" -b "$build_dir" -r l -t 10 -T 2 -u 4 -h WT_TEST.schema_disagg_abort.u4

# A lone follower generates its own workload and never checkpoints, so nothing becomes durable; the
# second run then steps up over the operations it accumulated, the way a fresh node bootstraps its
# own tables before taking leadership.
$TEST_WRAPPER "$test_bin" -b "$build_dir" -r f -t 10 -T 2 -h WT_TEST.schema_disagg_abort.f
$TEST_WRAPPER "$test_bin" -b "$build_dir" -r f -s 6 -t 18 -T 2 -h WT_TEST.schema_disagg_abort.fs

# Single-node role switches every 5 seconds, ending gracefully; each swap is a graceful step-down
# followed by a step-up over the node's own checkpoint.
$TEST_WRAPPER "$test_bin" -b "$build_dir" -r l -s 5 -t 20 -T 2 -h WT_TEST.schema_disagg_abort.ls

# The same, killed mid-run, so a crash can land next to a step-down.
$TEST_WRAPPER "$test_bin" -b "$build_dir" -r l -s 5 -k 12 -t 20 -T 2 -h WT_TEST.schema_disagg_abort.lsk

# Single-node crash: kill the lone node mid-run.
$TEST_WRAPPER "$test_bin" -b "$build_dir" -r l -k 8 -t 10 -T 2 -h WT_TEST.schema_disagg_abort.lk

# Two nodes: the follower applies the leader's events and picks up checkpoints.
$TEST_WRAPPER "$test_bin" -b "$build_dir" -r lf -t 10 -T 2 -h WT_TEST.schema_disagg_abort.lf
$TEST_WRAPPER "$test_bin" -b "$build_dir" -r lf -k l8 -t 12 -T 2 -h WT_TEST.schema_disagg_abort.kl
$TEST_WRAPPER "$test_bin" -b "$build_dir" -r lf -k f8 -t 12 -T 2 -h WT_TEST.schema_disagg_abort.kf
$TEST_WRAPPER "$test_bin" -b "$build_dir" -r lf -k l8 -k f8 -t 12 -T 2 -h WT_TEST.schema_disagg_abort.kb

# Two nodes with role swaps; after a kill the lone survivor still switches by sentinel.
$TEST_WRAPPER "$test_bin" -b "$build_dir" -r lf -s 5 -t 20 -T 2 -h WT_TEST.schema_disagg_abort.lfs
$TEST_WRAPPER "$test_bin" -b "$build_dir" -r lf -s 6 -k l9 -t 20 -T 2 -h WT_TEST.schema_disagg_abort.lfsk
