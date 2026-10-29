#!/usr/bin/env bash
set -euo pipefail

# Repo root from script location
cd "$(dirname "$0")"/..
WORKSPACE="$(pwd)"

# Use the hermetic Bazel GDB toolchain, which supports breakpoints in binaries
# built with split DWARF.
exec bazel run gdb -- \
    -iex "set auto-load safe-path $WORKSPACE/.gdbinit" \
    -iex "set substitute-path ./external $WORKSPACE/bazel-$(basename "$WORKSPACE")/external" \
    -iex "set substitute-path . $WORKSPACE" \
    -iex "set debug-file-directory $WORKSPACE" \
    "$@"
