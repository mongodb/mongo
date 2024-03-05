#!/bin/bash

# Needed for evergreen scripts that use evergreen expansions and utility methods.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

mkdir -p $TMPDIR

EXT=""
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" || "$OSTYPE" == "win32" || "$OSTYPE" == "win64" ]]; then
  OS="windows"
  EXT=".exe"
elif [[ "$OSTYPE" == "darwin"* ]]; then
  OS="darwin"
else
  OS="linux"
fi

ARCH=$(uname -m)
if [[ "$ARCH" == "arm64" || "$ARCH" == "aarch64" ]]; then
  ARCH="arm64"
elif [[ "$ARCH" == "ppc64le" || "$ARCH" == "ppc64" || "$ARCH" == "ppc" || "$ARCH" == "ppcle" ]]; then
  ARCH="ppc64le"
else
  ARCH="amd64"
fi

# TODO(SERVER-86050): remove the branch once bazelisk is built on s390x & ppc64le
if [[ $ARCH == "ppc64le" ]]; then
  REMOTE_PATH=https://mdb-build-public.s3.amazonaws.com/bazel-binaries/bazel-6.4.0-${ARCH}
  LOCAL_PATH=$TMPDIR/bazel
else
  REMOTE_PATH=https://mdb-build-public.s3.amazonaws.com/bazelisk-binaries/v1.19.0/bazelisk-${OS}-${ARCH}${EXT}
  LOCAL_PATH=$TMPDIR/bazelisk
fi

# Delete any previously downloaded bazel/bazelisk bin in case that would get in the way of the curl request below
if [ -f "$LOCAL_PATH" ]; then
  rm $LOCAL_PATH
fi

CURL_COMMAND="curl -L $REMOTE_PATH --output $LOCAL_PATH"

echo $CURL_COMMAND
eval $CURL_COMMAND

chmod +x $LOCAL_PATH
