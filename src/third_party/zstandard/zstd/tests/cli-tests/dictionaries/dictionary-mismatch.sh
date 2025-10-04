#!/bin/sh

. "$COMMON/platform.sh"

set -e

if [ false ]; then
	for seed in $(seq 100); do
		datagen -g1000 -s$seed > file$seed
	done

	zstd --train -r . -o dict0 -qq

	for seed in $(seq 101 200); do
		datagen -g1000 -s$seed > file$seed
	done

	zstd --train -r . -o dict1 -qq

	[ "$($MD5SUM < dict0)" != "$($MD5SUM < dict1)" ] || die "dictionaries must not match"

	datagen -g1000 -s0 > file0
fi

set -v
zstd files/0 -D dicts/0 -q
zstd -t files/0.zst -D dicts/0
zstd -t files/0.zst -D dicts/1 && die "Must fail" ||:
zstd -t files/0.zst            && die "Must fail" ||:
