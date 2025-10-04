#!/bin/sh
set -e
for i in $(seq 50); do
    datagen -s$i > file$i
done
touch empty

set -v
zstd -q --train empty file*
