#!/bin/bash
# Configures a cares suitable for use in the mongo server, built on the host machine.

set -euo pipefail
IFS=$'\n\t'

set -vx


DEST_DIR=$(git rev-parse --show-toplevel)/src/third_party/cares

HOST_OS="$(uname -s|tr A-Z a-z)"
HOST_ARCH="$(uname -m)"
HOST_DIR="$DEST_DIR/platform/${HOST_OS}_${HOST_ARCH}"


SRC_DIR=${DEST_DIR}/dist
pushd $SRC_DIR
autoreconf -fi
popd

mkdir -p $HOST_DIR/build
pushd $HOST_DIR/build

# force disable:
#   coredump : postmortem analysis of core memory dumps
#   ptrace : unwinding stacks in another process
#   setjmp : provides a nonlocal goto feature
#   documentation : won't need them
#   tests : won't need them
#   dependency-tracking : (from automake) disabled because we only do one build
#   cxx-exceptions : intrusive exception handling runtime
$SRC_DIR/configure \
    --prefix=$HOST_DIR/install \



make install
popd
