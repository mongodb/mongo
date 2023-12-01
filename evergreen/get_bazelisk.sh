#!/bin/bash

cd src

set -o errexit
set -o verbose

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
else
  ARCH="amd64"
fi

# TODO(SERVER-81038): remove once bazel/bazelisk is self-hosted.
CURL_COMMAND="curl -L https://github.com/bazelbuild/bazelisk/releases/download/v1.17.0/bazelisk-${OS}-${ARCH}${EXT} --output ./bazelisk"

echo $CURL_COMMAND
eval $CURL_COMMAND

chmod +x "./bazelisk"
