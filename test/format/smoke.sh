#! /bin/sh

# Smoke-test format as part of running "make check".
./t -1 -c "." data_source=table file_type=row ops=100000 rows=10000 threads=4
