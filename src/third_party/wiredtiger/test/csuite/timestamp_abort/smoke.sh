#! /bin/sh

set -e

# Smoke-test timestamp-abort as part of running "make check". Use the -s option
# to add a stress timing in checkpoint prepare.

default_test_args="-t 10 -T 5"
while getopts ":s" opt; do
    case $opt in
        s) default_test_args="$default_test_args -s" ;;
    esac
done


# If $top_builddir/$top_srcdir aren't set, default to building in build_posix
# and running in test/csuite.
top_builddir=${top_builddir:-../../build_posix}
top_srcdir=${top_srcdir:-../..}

$TEST_WRAPPER $top_builddir/test/csuite/test_timestamp_abort $default_test_args
#$TEST_WRAPPER $top_builddir/test/csuite/test_timestamp_abort $default_test_args -L
$TEST_WRAPPER $top_builddir/test/csuite/test_timestamp_abort -m $default_test_args
#$TEST_WRAPPER $top_builddir/test/csuite/test_timestamp_abort -m $default_test_args -L
$TEST_WRAPPER $top_builddir/test/csuite/test_timestamp_abort -C $default_test_args
$TEST_WRAPPER $top_builddir/test/csuite/test_timestamp_abort -C -m $default_test_args
