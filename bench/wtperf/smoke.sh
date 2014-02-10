#! /bin/sh

# Smoke-test wtperf as part of running "make check".
./wtperf -O ../../../bench/wtperf/runners/small-lsm.wtperf -o "run_time=20"
