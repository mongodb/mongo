#!/bin/bash
#
# The script fetches the sources for 'derickr/timelib', downloads the latest Olson timezone
# database and generates the necessary sources for the imports. The sources will be generated
# inside 'src/third_party/timelib/dist/' and should be commited to the repository.
#
# The library does not use any autotools/cmake/config system to it, as it is a simple import.
# This script is designed to run on Linux or Mac OS X.
#
# Usage: ./src/third_party/timelib/scripts/import.sh

VERSION=2022.13
NAME=timelib

set -o verbose
set -o errexit

if grep -q Microsoft /proc/version; then
    TEMP_DIR=$(wslpath -u $(powershell.exe -Command "Get-ChildItem Env:TEMP | Get-Content | Write-Host"))
else
    TEMP_DIR="/tmp"
fi
TEMP_DIR=$(mktemp -d $TEMP_DIR/$NAME.XXXXXX)
DEST_DIR=`git rev-parse --show-toplevel`/src/third_party/$NAME/dist

if [[ -d $DEST_DIR ]]; then
    echo "You must remove '$DEST_DIR' before running $0" >&2
    exit 1
fi

# Check prerequisites: re2c, wget and php.
if ! [ -x "$(command -v re2c)" ]; then
    echo 'Error: re2c is not installed.' >&2
    exit 1
fi

if ! [ -x "$(command -v wget)" ]; then
    echo 'Error: wget is not installed.' >&2
    exit 1
fi

if ! [ -x "$(command -v php)" ]; then
    echo 'Error: php is not installed.' >&2
    exit 1
fi

mkdir -p $DEST_DIR
pushd $TEMP_DIR
git clone --branch $VERSION --depth 1 git@github.com:derickr/timelib.git .

# Avoid embedding the git repository itself.
rm -rf .git*

# Prune unneeded files.
rm -rf tests/

# Download the latest Olson timezone database and generate 'timezonedb.h'.
make -C zones

# Generate the parsers.
make parse_date.c parse_iso_intervals.c

# Clean up the build artifacts and copy the end sources to the destination directory.
make -C zones clean
cp -r * $DEST_DIR
popd
echo "Done"
