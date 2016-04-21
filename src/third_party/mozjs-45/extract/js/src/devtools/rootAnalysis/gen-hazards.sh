#!/bin/sh

set -e

JOBS="$1"

for j in $(seq $JOBS); do
    env PATH=$PATH:$SIXGILL/bin XDB=$SIXGILL/bin/xdb.so $JS $ANALYZE gcFunctions.lst suppressedFunctions.lst gcTypes.txt $j $JOBS tmp.$j > rootingHazards.$j &
done

wait

for j in $(seq $JOBS); do
    cat rootingHazards.$j
done
