#!/usr/bin/env bash
set -e

db_contrib_tool="${PWD}/${DB_CONTRIB_TOOL}"

# Change to the workspace root so that db-contrib-tool's relative-path defaults
# land inside the repo rather than the bazel runfiles directory.
if [[ -n "${BUILD_WORKSPACE_DIRECTORY:-}" ]]; then
    cd "${BUILD_WORKSPACE_DIRECTORY}"
fi

exec "${db_contrib_tool}" "$@"
