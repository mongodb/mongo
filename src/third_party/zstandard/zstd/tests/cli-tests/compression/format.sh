#!/bin/sh

. "$COMMON/format.sh"

set -e

# Test --format
zstd --format=zstd file -f
zstd -t file.zst
for format in "gzip" "lz4" "xz" "lzma"; do
	if zstd_supports_format $format; then
		zstd --format=$format file
		zstd -t file.$(format_extension $format)
		zstd -c --format=$format file | zstd -t --format=$format
	fi
done
