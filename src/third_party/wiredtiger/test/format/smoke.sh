#! /bin/sh

set -e

# Smoke-test format as part of running "make check".
args="-1 -c "." data_source=table ops=50000 rows=10000 threads=4 compression=none logging_compression=none"

$TEST_WRAPPER ./t $args file_type=fix
$TEST_WRAPPER ./t $args file_type=row
$TEST_WRAPPER ./t $args file_type=row data_source=lsm
$TEST_WRAPPER ./t $args file_type=var
# Force a rebalance to occur with statistics logging to test the utility
$TEST_WRAPPER ./t $args file_type=row statistics_server=1 rebalance=1
