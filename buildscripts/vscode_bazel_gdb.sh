#!/usr/bin/env bash
set -euo pipefail

# Repo root from script location
cd "$(dirname "$0")"/..
WORKSPACE="$(pwd)"

# TODO(SERVER-127579): The current bazel gdb_v5 target produces a GDB with a bug
# preventing breakpoints from being set for binaries that are built with split DWARF.
# Revert back to bazel gdb_v5 when the target builds an upgraded GDB.
exec /opt/mongodbtoolchain/v5/bin/gdb \
    -iex "set auto-load safe-path $WORKSPACE/.gdbinit" \
    -iex "set substitute-path ./external $WORKSPACE/bazel-$(basename "$WORKSPACE")/external" \
    -iex "set substitute-path . $WORKSPACE" \
    -iex "set debug-file-directory $WORKSPACE" \
    "$@"
