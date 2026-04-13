#!/usr/bin/env bash

# Collects one or more bazel jvm.out snapshots into a task-local tarball and
# can optionally request a fresh dump from any live bazel server processes
# first.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"
. "$DIR/bazel_evergreen_shutils.sh"

set -o errexit
set -o pipefail

signal_bazel_quit=false
if [[ "${1:-}" == "--signal-bazel-quit" ]]; then
    signal_bazel_quit=true
    shift
fi

if [[ "$#" -ne 0 ]]; then
    echo "Usage: $0 [--signal-bazel-quit]" >&2
    exit 1
fi

cd src

BAZEL_BINARY="$(bazel_evergreen_shutils::bazel_get_binary_path)"
ARCHIVE_PATH="jvm.out.tar.gz"

if $signal_bazel_quit; then
    bazel_evergreen_shutils::request_bazel_jvm_dump "$BAZEL_BINARY" || true
    exit 0
fi

rm -f "$ARCHIVE_PATH"
bazel_evergreen_shutils::package_bazel_jvm_out "$BAZEL_BINARY" "$ARCHIVE_PATH" || true
