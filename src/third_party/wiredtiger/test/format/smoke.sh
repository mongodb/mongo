#! /bin/sh

set -e

# Smoke-test format as part of running "make check".
args="-1 -c . "
args="$args btree.compression=none "
args="$args cache.minimum=40 "
args="$args logging_compression=none"
args="$args runs.ops=500000 "
args="$args runs.rows=100000 "
args="$args runs.source=table "
args="$args runs.threads=4 "

# Temporarily disable LSM and FLCS.
# $TEST_WRAPPER ./t $args runs.type=fix
# $TEST_WRAPPER ./t $args runs.type=row runs.source=lsm

$TEST_WRAPPER ./t $args runs.type=row
$TEST_WRAPPER ./t $args runs.type=var
