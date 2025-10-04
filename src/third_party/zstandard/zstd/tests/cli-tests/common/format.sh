#!/bin/sh

. "$COMMON/platform.sh"

zstd_supports_format()
{
	zstd -h | grep > $INTOVOID -- "--format=$1"
}

format_extension()
{
	if [ "$1" = "zstd" ]; then
		printf "zst"
	elif [ "$1" = "gzip" ]; then
		printf "gz"
	else
		printf "$1"
	fi
}
