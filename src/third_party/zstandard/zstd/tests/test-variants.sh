#!/bin/sh
set -e
set -u
set -x


SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROG_DIR="$SCRIPT_DIR/../programs"

ZSTD="$PROG_DIR/zstd"
ZSTD_COMPRESS="$PROG_DIR/zstd-compress"
ZSTD_DECOMPRESS="$PROG_DIR/zstd-decompress"
ZSTD_NOLEGACY="$PROG_DIR/zstd-nolegacy"
ZSTD_DICTBUILDER="$PROG_DIR/zstd-dictBuilder"
ZSTD_FRUGAL="$PROG_DIR/zstd-frugal"
ZSTD_NOMT="$PROG_DIR/zstd-nomt"

println() {
    printf '%b\n' "${*}"
}

die() {
    println "$@" 1>&2
    exit 1
}

symbol_present() {
	(nm $1 || echo "symbol_present $@ failed") | grep $2
}

symbol_not_present() {
	symbol_present $@ && die "Binary '$1' mistakenly contains symbol '$2'" ||:
}

compress_not_present() {
	symbol_not_present "$1" ZSTD_compress
}

decompress_not_present() {
	symbol_not_present "$1" ZSTD_decompress
}

dict_not_present() {
	symbol_not_present "$1" ZDICT_
	symbol_not_present "$1" COVER_
}

cliextra_not_present() {
	symbol_not_present "$1" TRACE_
	symbol_not_present "$1" BMK_
}

legacy_not_present() {
	symbol_not_present "$1" ZSTDv0
}

test_help() {
	"$1" --help | grep -- "$2"
}

test_no_help() {
	test_help $@ && die "'$1' supports '$2' when it shouldn't" ||:
}

extras_not_present() {
	dict_not_present $@
	legacy_not_present $@
	cliextra_not_present $@
	test_no_help $@ "--train"
	test_no_help $@ "-b#"
}

test_compress() {
	echo "hello" | "$1" | "$ZSTD" -t
}

test_decompress() {
	echo "hello" | "$ZSTD" | "$1" -t
}

test_zstd() {
	test_compress $@
	test_decompress $@
}

extras_not_present "$ZSTD_FRUGAL"
extras_not_present "$ZSTD_COMPRESS"
extras_not_present "$ZSTD_DECOMPRESS"

compress_not_present "$ZSTD_DECOMPRESS"

decompress_not_present "$ZSTD_COMPRESS"
decompress_not_present "$ZSTD_DICTBUILDER"

cliextra_not_present "$ZSTD_DICTBUILDER"

legacy_not_present "$ZSTD_DICTBUILDER"
legacy_not_present "$ZSTD_NOLEGACY"

symbol_not_present "$ZSTD" ZSTDv01
symbol_not_present "$ZSTD" ZSTDv02
symbol_not_present "$ZSTD" ZSTDv03
symbol_not_present "$ZSTD" ZSTDv04

test_compress "$ZSTD_COMPRESS"
test_decompress "$ZSTD_DECOMPRESS"

test_zstd "$ZSTD_FRUGAL"
test_zstd "$ZSTD_NOLEGACY"

test_help "$ZSTD" '-b#'
test_help "$ZSTD" --train
test_help "$ZSTD_DICTBUILDER" --train

println "Success!"
