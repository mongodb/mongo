#! /bin/sh

# Smoke-test format as part of running "make check".
args="-1 -c "." data_source=table ops=100000 rows=10000 threads=4 compression=none"

./t $args file_type=fix || exit 1
./t $args file_type=row || exit 1
./t $args file_type=row data_source=lsm || exit 1
./t $args file_type=var || exit 1
