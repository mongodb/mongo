#! /bin/sh

set -e

# Smoke-test cursors as part of running "make check".

echo "cursor_order: rows"
$TEST_WRAPPER ./cursor_order -tr

echo "cursor_order: variable-length columns"
$TEST_WRAPPER ./cursor_order -tv
