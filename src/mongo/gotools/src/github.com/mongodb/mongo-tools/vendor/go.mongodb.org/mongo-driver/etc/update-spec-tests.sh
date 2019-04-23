#!/usr/bin/env bash

# This script is used to fetch the latest JSON tests for the CRUD spec. It puts the tests in the
# direcory $reporoot/data/crud. It should be run from the root of the repository.

set -o errexit
set -o nounset

if [ ! -d ".git" ]; then
    echo "$0: This script must be run from the root of the repository" >&2
    exit 1
fi

if [ $# -ne 1 ]; then
    echo "$0: This script must be passed exactly one argument for which tests to sync" >&2
    exit 1
fi

tmpdir=`perl -MFile::Temp=tempdir -wle 'print tempdir(TMPDIR => 1, CLEANUP => 0)'`
curl -sL https://github.com/mongodb/specifications/archive/master.zip -o "$tmpdir/specs.zip"
unzip -d "$tmpdir" "$tmpdir/specs.zip" > /dev/null
mkdir -p "data/$1"
rsync -ah "$tmpdir/specifications-master/source/$1/tests/" "data/$1"
rm -rf "$tmpdir"
