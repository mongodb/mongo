#!/bin/sh -e

die() {
    $ECHO "$@" 1>&2
    exit 1
}

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

INTOVOID="/dev/null"
case "$OS" in
  Windows*)
    INTOVOID="NUL"
    ;;
esac

ZSTD_LIB_COMPRESSION=0 CFLAGS= make -C $DIR/../lib libzstd.a > $INTOVOID
nm $DIR/../lib/libzstd.a | grep ".*\.o:" > tmplog
! grep -q "zstd_compress" tmplog && grep -q "zstd_decompress" tmplog && ! grep -q "dict" tmplog && grep -q "zstd_v" tmplog && ! grep -q "zbuff" tmplog && make clean && rm -f tmplog || die "Compression macro failed"


ZSTD_LIB_DECOMPRESSION=0 CFLAGS= make -C $DIR/../lib libzstd.a > $INTOVOID
nm $DIR/../lib/libzstd.a | grep ".*\.o:" > tmplog
grep -q "zstd_compress" tmplog && ! grep -q "zstd_decompress" tmplog && grep -q "dict" tmplog && ! grep -q "zstd_v" tmplog && ! grep -q "zbuff" tmplog && make clean && rm -f tmplog || die "Decompression macro failed"

ZSTD_LIB_DEPRECATED=0 CFLAGS= make -C $DIR/../lib libzstd.a > $INTOVOID
nm $DIR/../lib/libzstd.a | grep ".*\.o:" > tmplog
grep -q "zstd_compress" tmplog && grep -q "zstd_decompress" tmplog && grep -q "dict" tmplog && grep -q "zstd_v" tmplog && ! grep -q "zbuff" tmplog && make clean && rm -f tmplog || die "Deprecated macro failed"

ZSTD_LIB_DICTBUILDER=0 CFLAGS= make -C $DIR/../lib libzstd.a > $INTOVOID
nm $DIR/../lib/libzstd.a | grep ".*\.o:" > tmplog
grep -q "zstd_compress" tmplog && grep -q "zstd_decompress" tmplog && ! grep -q "dict" tmplog && grep -q "zstd_v" tmplog && grep -q "zbuff" tmplog && make clean && rm -f tmplog || die "Dictbuilder macro failed"

ZSTD_LIB_DECOMPRESSION=0 ZSTD_LIB_DICTBUILDER=0 CFLAGS= make -C $DIR/../lib libzstd.a > $INTOVOID
nm $DIR/../lib/libzstd.a | grep ".*\.o:" > tmplog
grep -q "zstd_compress" tmplog && ! grep -q "zstd_decompress" tmplog && ! grep -q "dict" tmplog && ! grep -q "zstd_v" tmplog && ! grep -q "zbuff" tmplog && make clean && rm -f tmplog || die "Multi-macro failed"