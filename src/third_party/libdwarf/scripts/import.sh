#!/bin/bash
# This script downloads and imports libdwarf

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=libdwarf
VERSION="2.1.0"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/libdwarf
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

mkdir -p $DEST_DIR/dist

wget -P $DEST_DIR/dist https://github.com/davea42/libdwarf-code/releases/download/v$VERSION/libdwarf-$VERSION.tar.xz
tar --strip-components=1 -xvJf $DEST_DIR/dist/libdwarf-$VERSION.tar.xz -C $DEST_DIR/dist

pushd $DEST_DIR/dist
find . -mindepth 1 -maxdepth 1 -name ".*" -exec rm -rf {} \;
rm -rf src/{bin,lib/libdwarfp}
rm -rf src/lib/libdwarf/{CMakeLists.txt,cmake}
rm -rf autom4te.cache bugxml benchmarking ci cmake doc fuzz m4 scripts tools test
find . -maxdepth 1 -type f -not -regex ".*\(AUTHORS\|ChangeLog\|config.h\|COPYING\|INSTALL\|NEWS\|LICENSE\|README.*\)$" -delete
popd

