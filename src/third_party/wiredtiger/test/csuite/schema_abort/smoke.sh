#! /bin/sh

set -e

# Smoke-test schema-abort as part of running "make check".

# If $top_builddir/$top_srcdir aren't set, default to building in build_posix
# and running in test/csuite.
top_builddir=${top_builddir:-../../build_posix}
top_srcdir=${top_srcdir:-../..}

$TEST_WRAPPER $top_builddir/test/csuite/test_schema_abort -t 10 -T 5
$TEST_WRAPPER $top_builddir/test/csuite/test_schema_abort -m -t 10 -T 5
$TEST_WRAPPER $top_builddir/test/csuite/test_schema_abort -C -t 10 -T 5
$TEST_WRAPPER $top_builddir/test/csuite/test_schema_abort -C -m -t 10 -T 5
$TEST_WRAPPER $top_builddir/test/csuite/test_schema_abort -m -t 10 -T 5 -z
$TEST_WRAPPER $top_builddir/test/csuite/test_schema_abort -m -t 10 -T 5 -x
