#! /bin/sh

set -e

# Smoke-test checkpoints as part of running "make check".

# 1. Mixed tables cases. Use four (or eight) tables because there are four table types.

echo "checkpoint: 4 mixed tables"
$TEST_WRAPPER ./t -t m -T 4

echo "checkpoint: 8 mixed tables"
$TEST_WRAPPER ./t -t m -T 8

echo "checkpoint: 4 mixed tables, with sweep"
$TEST_WRAPPER ./t -t m -T 4 -W 3 -r 2 -n 100000 -k 100000 -s 1

echo "checkpoint: 4 mixed tables, with timestamps"
$TEST_WRAPPER ./t -t m -T 4 -W 3 -r 2 -n 100000 -k 100000 -x

# 2. FLCS cases.

echo "checkpoint: 6 fixed-length column-store tables"
$TEST_WRAPPER ./t -t f -T 6

echo "checkpoint: 6 fixed-length column-store tables, named checkpoint"
$TEST_WRAPPER ./t -t f -T 6 -c 'TeSt'

echo "checkpoint: 6 fixed-length column-store tables with prepare"
$TEST_WRAPPER ./t -t f -T 6 -p

echo "checkpoint: 6 fixed-length column-store tables, named checkpoint with prepare"
$TEST_WRAPPER ./t -t f -T 6  -c 'TeSt' -p

echo "checkpoint: fixed-length column-store tables, stress history store. Sweep and timestamps"
$TEST_WRAPPER ./t -t f -W 3 -r 2 -s 1 -x -n 100000 -k 100000 -C cache_size=100MB -D

echo "checkpoint: fixed-length column-store tables, Sweep and timestamps"
$TEST_WRAPPER ./t -t f -W 3 -r 2 -s 1 -x -n 100000 -k 100000 -C cache_size=100MB

# 3. VLCS cases.

echo "checkpoint: 6 column-store tables"
$TEST_WRAPPER ./t -t c -T 6

echo "checkpoint: 6 column-store tables, named checkpoint"
$TEST_WRAPPER ./t -t c -T 6 -c 'TeSt'

echo "checkpoint: 6 column-store tables with prepare"
$TEST_WRAPPER ./t -t c -T 6 -p

echo "checkpoint: 6 column-store tables, named checkpoint with prepare"
$TEST_WRAPPER ./t -t c -T 6 -c 'TeSt' -p

echo "checkpoint: column-store tables, stress history store. Sweep and timestamps"
$TEST_WRAPPER ./t -t c -W 3 -r 2 -s 1 -x -n 100000 -k 100000 -C cache_size=100MB -D

echo "checkpoint: column-store tables, Sweep and timestamps"
$TEST_WRAPPER ./t -t c -W 3 -r 2 -s 1 -x -n 100000 -k 100000 -C cache_size=100MB

# 4. Row-store cases.

echo "checkpoint: 6 row-store tables"
$TEST_WRAPPER ./t -t r -T 6

echo "checkpoint: 6 row-store tables, named checkpoint"
$TEST_WRAPPER ./t -t r -T 6 -c 'TeSt'

echo "checkpoint: 6 row-store tables with prepare"
$TEST_WRAPPER ./t -t r -T 6 -p

echo "checkpoint: 6 row-store tables, named checkpoint with prepare"
$TEST_WRAPPER ./t -t r -T 6 -c 'TeSt' -p

echo "checkpoint: row-store tables, stress history store. Sweep and timestamps"
$TEST_WRAPPER ./t -t r -W 3 -r 2 -s 1 -x -n 100000 -k 100000 -C cache_size=100MB -D

echo "checkpoint: row-store tables, Sweep and timestamps"
$TEST_WRAPPER ./t -t r -W 3 -r 2 -s 1 -x -n 100000 -k 100000 -C cache_size=100MB

# 5. LSM cases.

echo "checkpoint: 6 LSM tables"
$TEST_WRAPPER ./t -t l -T 6
