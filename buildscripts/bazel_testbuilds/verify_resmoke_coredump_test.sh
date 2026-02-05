#!/bin/bash
#
# Test script to verify that the test timeouts in resmoke_suite_test generate coredumps and are picked up for core analysis tasks.
#
# This script:
#  1. Runs a `bazel test` on a resmoke suite that is expected to fail and generate a core.
#  2. Runs evergreen/fetch_remote_test_results.sh, which downloads test outputs for the remotely executed test.
#  3. Runs the gen_hang_analyzer_tasks script that generates an Evergreen task config for core analysis.
#
# Usage:
#   ./buildscripts/bazel_testbuilds/verify_resmoke_coredump_test.sh
#
# Exit codes:
#   0 - Success (coredump and generate task config were created)
#   1 - Failure

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Change to repo root for bazel commands
cd "$REPO_ROOT" || exit 1

TEST_TARGET="//buildscripts/bazel_testbuilds:jstest_timeout"

# Cleanup function to remove temp directories
TEMP_DIR=""
cleanup() {
    # Clean up temp directory if it was created
    if [[ -n "${TEMP_DIR}" && -d "${TEMP_DIR}" ]]; then
        rm -rf "${TEMP_DIR}" 2>/dev/null || true
    fi
    bazel shutdown
}
trap cleanup EXIT

echo "=== Coredump Generation Verification Test ==="
echo "Test target: ${TEST_TARGET}"
echo "Repository root: ${REPO_ROOT}"
echo ""

# Run the bazel test (expected to fail). --platforms is used so the test runs in the same environment regardless of the host.
echo "Running bazel test (this test is expected to fail)..."
echo "Command: bazel test --config=remote_test --zip_undeclared_test_outputs --build_event_json_file=build_events.json ${TEST_TARGET}"
echo ""

# Use --curses=no and --color=no to prevent interactive output that might cause hangs in CI.
bazel test --config=remote_test --zip_undeclared_test_outputs --build_event_json_file=build_events.json --curses=no --color=no "${TEST_TARGET}" 2>&1 && BAZEL_EXIT_CODE=0 || BAZEL_EXIT_CODE=$?

echo ""
echo "Bazel test exit code: ${BAZEL_EXIT_CODE}"
echo ""

# The test should fail (exit code != 0)
if [[ "${BAZEL_EXIT_CODE}" -eq 0 ]]; then
    echo "ERROR: Test unexpectedly passed. The timeout mechanism may not have triggered."
    exit 1
fi

echo "Test failed as expected. Now fetching remote test results..."
echo ""

# Fetch the remote test results. In reality this would be run a on different host than the one that ran `bazel test`.
TEMP_DIR=$(mktemp -d)
export ENGFLOW_KEY="${workdir}/src/engflow.key"
export ENGFLOW_CERT="${workdir}/src/engflow.cert"
export workdir="$TEMP_DIR" # Change workdir so the script downloads outputs to the temporary dir, rather than task workdir.
export test_label="$TEST_TARGET"
bash ./evergreen/fetch_remote_test_results.sh
echo ""
unset workdir # Unset workdir, it's a default Evergreen expansion that might confuse a later script.

OUTPUTS_DIR="${TEMP_DIR}"/results/buildscripts/bazel_testbuilds/jstest_timeout/shard_1/test.outputs
# List all files in the test output directory for debugging.
if [[ -d "${OUTPUTS_DIR}" ]]; then
    echo "Contents of ${OUTPUTS_DIR}:"
    find "${OUTPUTS_DIR}" -type f 2>/dev/null | head -50
    echo ""
else
    echo "FAILED: Test output directory not found: ${OUTPUTS_DIR}"
    exit 1
fi

# Look for the expected core file.
echo "Searching for coredump files in ${OUTPUTS_DIR}..."
CORE_FILES=$(find "${OUTPUTS_DIR}" -type f \( -name "*.core" -o -name "*.core.gz" -o -name "dump_*.core*" \) 2>/dev/null)
COREDUMP_FOUND=0
COREDUMP_FILE=""
if [[ -n "${CORE_FILES}" ]]; then
    COREDUMP_FOUND=1
    COREDUMP_FILE=$(echo "${CORE_FILES}" | head -1)
    echo "SUCCESS: Coredump file(s) found:"
    echo "${CORE_FILES}"
else
    echo "FAILED: No coredump files found."
    exit 1
fi

# Create an expansions file that is like what will exist in the tests tasks.
EXPANSIONS_FILE="${TEMP_DIR}/expansions.yml"
cat <<EOF >"${EXPANSIONS_FILE}"
    core_analyzer_distro_name: amazon2023-arm64-atlas-latest-m8g-2xlarge
    task_name: "${TEST_TARGET}"
    task_id: task_id_123
    execution: 0
    build_variant: build_variant_123
    core_analyzer_results_url: https://core_analyzer_results_url
    workdir: "${TEMP_DIR}"
EOF

GENERATED_TASK_FILE="${TEMP_DIR}/generated_tasks.json"
bazel run //buildscripts/resmokelib/hang_analyzer:gen_hang_analyzer_tasks --config=remote_test -- --expansions-file="${EXPANSIONS_FILE}" --output-file="${GENERATED_TASK_FILE}" --tests-use-bazel --use-mock-tasks

if [[ -f "${GENERATED_TASK_FILE}" ]]; then
    echo "SUCCESS: Created the Evergreen task config ${GENERATED_TASK_FILE}"
    cat "${GENERATED_TASK_FILE}"
    echo ""
else
    echo "FAILED: Did not generate an Evergreen task config at ${GENERATED_TASK_FILE}"
    exit 1
fi
