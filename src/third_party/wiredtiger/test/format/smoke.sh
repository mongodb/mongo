#! /bin/sh

set -e

# Smoke-test format as part of running "make check".
args="-c . "
args="$args btree.compression=none "
args="$args cache.minimum=40 "
args="$args logging_compression=none"
args="$args runs.rows=100000 "
args="$args runs.source=table "
args="$args runs.tables=3 "
args="$args runs.threads=6 "
args="$args runs.timer=1 "
args="$args transaction.timestamps=1 "

# Temporarily disable LSM and FLCS.
# $TEST_WRAPPER ./t $args runs.type=fix
# $TEST_WRAPPER ./t $args runs.type=row runs.source=lsm

$TEST_WRAPPER ./t $args runs.type=row
$TEST_WRAPPER ./t $args runs.type=var
