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
    # If $top_builddir/$top_srcdir aren't set, default to building in build_posix
    # and running in test/csuite.
    top_builddir=${top_builddir:-../../build_posix}
    top_srcdir=${top_srcdir:-../..}
    test_bin=$top_builddir/test/csuite/test_wt2909_checkpoint_integrity
fi

$TEST_WRAPPER $test_bin $builddir_arg -t r
$TEST_WRAPPER $test_bin $builddir_arg -t c
