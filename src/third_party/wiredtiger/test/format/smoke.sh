#! /bin/sh

set -e

# Smoke-test format as part of running "make check".
args="$args btree.compression=none "
args="$args logging_compression=none"
args="$args runs.ops=50000 "
args="$args runs.rows=10000 "
args="$args runs.source=table "
args="$args runs.threads=4 "

# Locate format.sh from home directory.
FORMAT_SCRIPT=$(git rev-parse --show-toplevel)/test/format/format.sh

# Temporarily disabled
# $FORMAT_SCRIPT -t 2 $args runs.type=fix
# $FORMAT_SCRIPT -t 2 $args runs.type=row runs.source=lsm
# $FORMAT_SCRIPT -t 2 $args runs.type=var

# Run the format script for 10 minutues, distribute it across the number
# of different test arguments.

# This will run the test/format binary for 5 minutes each.
$FORMAT_SCRIPT -t 5 $args runs.type=row

$FORMAT_SCRIPT -t 5 $args runs.type=row statistics.server=1 ops.rebalance=1
