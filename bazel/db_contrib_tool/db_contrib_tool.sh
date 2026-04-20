#!/usr/bin/env bash
set -e

runfiles_root="$(pwd)"
db_contrib_tool="${runfiles_root}/../db_contrib_tool/db-contrib-tool"

# Change to the workspace root so that db-contrib-tool's relative-path defaults
# land inside the repo rather than the bazel runfiles directory.
if [[ -n "${BUILD_WORKSPACE_DIRECTORY:-}" ]]; then
    cd "${BUILD_WORKSPACE_DIRECTORY}"
fi

exec "${db_contrib_tool}" "$@"
