#!/bin/bash

# This script downloads and imports croaring

set -euo pipefail
IFS=$'\n\t'

set -vx

NAME=croaring
REVISION="v2.1.2"
VERSION="v2.1.2"

DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/croaring
if [[ -d $DEST_DIR/dist ]]; then
    echo "You must remove '$DEST_DIR/dist' before running $0" >&2
    exit 1
fi

git clone --branch $REVISION git@github.com:mongodb-forks/CRoaring.git $DEST_DIR/dist
pushd $DEST_DIR/dist

./amalgamation.sh

mv roaring.c ..
mv roaring.h ..
mv roaring.hh ..
cd ..
rm -rf dist
mkdir dist
cd dist
mv ../roaring.c .
mv ../roaring.h .
mv ../roaring.hh .
sed -i 's/\/\/\sCreated\sby\samalgamation.sh\son\s.*/\/\/ Created by amalgamation.sh/g' *
popd
