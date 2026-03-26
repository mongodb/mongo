#!/bin/bash
# report_version_set_results.sh — task teardown: report server+mongot e2e results to URP
#
# Called at task teardown when a task may have used urpcli versionset materialize.
# Silently skips if version_set.yaml is not present (task did not use version sets).
# Does NOT fail the task if report-results fails — this is best-effort reporting.
#
# Expected env vars (set by Evergreen):
#   task_exit_code  — the exit code of the test step (default: 0 if unset)

set -o pipefail

VERSION_SET_YAML="${workdir:-$PWD}/version_set.yaml"

if [[ ! -f "${VERSION_SET_YAML}" ]]; then
    echo "report_version_set_results.sh: version_set.yaml not found — skipping result report"
    exit 0
fi

URPCLI_BIN="${workdir:-$HOME}/bin/urpcli"
if [[ ! -f "${URPCLI_BIN}" ]]; then
    # Fall back to PATH
    URPCLI_BIN="urpcli"
fi

_exit_code="${task_exit_code:-0}"

echo "Reporting version set results: test-name=server-mongot-e2e exit-code=${_exit_code}"
"${URPCLI_BIN}" versionset report-results \
    --test-name server-mongot-e2e \
    --exit-code "${_exit_code}" || {
    echo "report_version_set_results.sh: urpcli versionset report-results failed (non-fatal)" >&2
}

exit 0
