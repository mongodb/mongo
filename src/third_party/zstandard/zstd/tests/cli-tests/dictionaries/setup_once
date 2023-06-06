#!/bin/sh

set -e

. "$COMMON/platform.sh"


mkdir files/ dicts/

for seed in $(seq 50); do
	datagen -g1000 -s$seed > files/$seed
done

zstd --train -r files -o dicts/0 -qq

for seed in $(seq 51 100); do
	datagen -g1000 -s$seed > files/$seed
done

zstd --train -r files -o dicts/1 -qq

cmp dicts/0 dicts/1 && die "dictionaries must not match!"

datagen -g1000 > files/0
