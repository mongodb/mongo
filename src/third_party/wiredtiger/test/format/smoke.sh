#! /bin/sh

set -e

# Smoke-test format as part of running "make check".
args="-1 -c . "
args="$args btree.compression=none "
args="$args logging_compression=none"
args="$args runs.ops=50000 "
args="$args runs.rows=10000 "
args="$args runs.source=table "
args="$args runs.threads=4 "

# Temporarily disabled
# $TEST_WRAPPER ./t $args runs.type=fix
# $TEST_WRAPPER ./t $args runs.type=row runs.source=lsm
# $TEST_WRAPPER ./t $args runs.type=var

$TEST_WRAPPER ./t $args runs.type=row
# Force a rebalance to occur with statistics logging to test the utility
# FIXME-WT-6410: temporarily disable running rebalance with timestamps
# $TEST_WRAPPER ./t $args runs.type=row statistics.server=1 ops.rebalance=1
