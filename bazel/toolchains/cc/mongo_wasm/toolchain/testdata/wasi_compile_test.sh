#!/usr/bin/env bash

set -o errexit
set -o nounset
set -o pipefail

wasm="$TEST_SRCDIR/$TEST_WORKSPACE/$1"
magic="$(od -An -tx1 -N4 "$wasm" | tr -d '[:space:]')"

if [[ "$magic" != "0061736d" ]]; then
    echo "Expected a WebAssembly binary, got magic bytes: $magic" >&2
    exit 1
fi
