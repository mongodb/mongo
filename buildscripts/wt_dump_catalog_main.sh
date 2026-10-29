#!/usr/bin/env bash
# Entrypoint for `bazel run //:wt_dump_catalog`.
# Sources the shared wt_functions.sh and invokes dump_catalog with the passed args.
set -euo pipefail

# Bazel sets BUILD_WORKSPACE_DIRECTORY to the repo root when invoked via `bazel run`.
: "${BUILD_WORKSPACE_DIRECTORY:?must be run via 'bazel run', which sets BUILD_WORKSPACE_DIRECTORY}"
export WORKSPACE_FOLDER="$BUILD_WORKSPACE_DIRECTORY"

# shellcheck source=/dev/null
source "$WORKSPACE_FOLDER/buildscripts/wt_functions.sh"

# `bazel run` executes from the runfiles dir, so cd back to the user's invocation
# directory (BUILD_WORKING_DIRECTORY) to make relative path args resolve correctly.
cd "${BUILD_WORKING_DIRECTORY:-$PWD}"

dump_catalog "$@"
