#!/bin/bash
# This script downloads and imports libfmt.

set -euo pipefail
IFS=$'\n\t'

set -vx

if [[ "$#" -ne 0 ]]; then
    echo "This script does not take any arguments" >&2
    exit 1
fi

NAME=fmt
REVISION=mongodb

# If WSL, get Windows temp directory
if $(grep -q Microsoft /proc/version); then
    TEMP_ROOT=$(wslpath -u $(powershell.exe -Command "Get-ChildItem Env:TEMP | Get-Content | Write-Host"))
else
    TEMP_ROOT="/tmp"
fi
GITDIR=$(mktemp -d $TEMP_ROOT/$NAME.XXXXXX)
trap "rm -rf $GITDIR" EXIT

DIST=$(git rev-parse --show-toplevel)/src/third_party/$NAME/dist

git clone "git@github.com:BillyDonahue/fmt.git" -c core.autocrlf=false $GITDIR
git -C $GITDIR checkout $REVISION

mkdir -p $DIST
cp -Trp $GITDIR/src $DIST/src

mkdir -p $DIST/include/fmt
cp -Trp $GITDIR/include/fmt $DIST/include/fmt
