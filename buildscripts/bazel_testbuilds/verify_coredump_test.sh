#!/bin/bash
#
# Test script to verify that the ci_wrapper timeout mechanism generates coredumps.
#
# This script:
#   1. Runs the ci_wrapper_timeout_test via bazel with --config=remote_test
#   2. Verifies that a coredump file is created in the test outputs
#
# Usage:
#   ./buildscripts/bazel_testbuilds/verify_coredump_test.sh
#
# Exit codes:
#   0 - Success (coredump was created)
#   1 - Failure (no coredump found or unexpected error)

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Change to repo root for bazel commands
cd "$REPO_ROOT" || exit 1

TEST_TARGET="//buildscripts/bazel_testbuilds:ci_wrapper_timeout_test"
TESTLOGS_PATH="bazel-testlogs/buildscripts/bazel_testbuilds/ci_wrapper_timeout_test"

# Cleanup function to restore modified files and remove temp directories
TEMP_DIR=""
cleanup() {
    # Restore the original timeout in test_wrapper.sh
    if [[ -f "bazel/test_wrapper.sh" ]]; then
        sed -i 's/timeout_seconds=5/timeout_seconds=600/' bazel/test_wrapper.sh 2>/dev/null || true
    fi
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

# Clean any previous test outputs
echo "Cleaning previous test outputs..."
rm -rf "${TESTLOGS_PATH}" 2>/dev/null || true

# Modify the timeout in test_wrapper.sh to 5s for faster test execution
echo "Modifying timeout in bazel/test_wrapper.sh from 600s to 5s..."
sed -i 's/timeout_seconds=600/timeout_seconds=5/' bazel/test_wrapper.sh

# Run the bazel test (expected to fail due to SIGABRT)
echo "Running bazel test (this test is expected to fail with SIGABRT)..."
echo "Command: bazel test --config=remote_test ${TEST_TARGET}"
echo ""

# The test is expected to fail (non-zero exit code) because it gets killed by SIGABRT
# We still want to continue and check for coredump
# Use --curses=no and --color=no to prevent interactive output that might cause hangs in CI
bazel test --config=remote_test --run_under=//bazel:test_wrapper --curses=no --color=no "${TEST_TARGET}" 2>&1 && BAZEL_EXIT_CODE=0 || BAZEL_EXIT_CODE=$?

echo ""
echo "Bazel test exit code: ${BAZEL_EXIT_CODE}"
echo ""

# The test should fail (exit code != 0) because it's killed by SIGABRT
if [[ "${BAZEL_EXIT_CODE}" -eq 0 ]]; then
    echo "ERROR: Test unexpectedly passed. The timeout mechanism may not have triggered."
    exit 1
fi

echo "Test failed as expected (SIGABRT). Now checking for coredump..."
echo ""

# Check for coredump in test outputs
# Coredumps can be in:
#   1. test.outputs/ directory (unzipped)
#   2. test.outputs__outputs.zip (zipped)

COREDUMP_FOUND=0
COREDUMP_FILE=""

# First, check if there's a zip file and unzip it
OUTPUTS_ZIP="${TESTLOGS_PATH}/test.outputs__outputs.zip"
OUTPUTS_DIR="${TESTLOGS_PATH}/test.outputs"

if [[ -f "${OUTPUTS_ZIP}" ]]; then
    echo "Found outputs zip: ${OUTPUTS_ZIP}"
    mkdir -p "${OUTPUTS_DIR}"
    unzip -o -q "${OUTPUTS_ZIP}" -d "${OUTPUTS_DIR}"
    echo "Unzipped to: ${OUTPUTS_DIR}"
fi

# Search for coredump files (.core or .core.gz)
echo ""
echo "Searching for coredump files in ${TESTLOGS_PATH}..."
echo ""

# List all files in the test output directory for debugging
if [[ -d "${TESTLOGS_PATH}" ]]; then
    echo "Contents of ${TESTLOGS_PATH}:"
    find "${TESTLOGS_PATH}" -type f 2>/dev/null | head -50
    echo ""
else
    echo "WARNING: Test logs directory not found: ${TESTLOGS_PATH}"
fi

# Look for core files
CORE_FILES=$(find "${TESTLOGS_PATH}" -type f \( -name "*.core" -o -name "*.core.gz" -o -name "dump_*.core*" \) 2>/dev/null)

if [[ -n "${CORE_FILES}" ]]; then
    COREDUMP_FOUND=1
    COREDUMP_FILE=$(echo "${CORE_FILES}" | head -1)
    echo "SUCCESS: Coredump file(s) found:"
    echo "${CORE_FILES}"
else
    echo "No coredump files found with standard patterns."
    # Also check for any file containing 'core' in the name
    echo ""
    echo "Checking for any files with 'core' in the name:"
    find "${TESTLOGS_PATH}" -type f -name "*core*" 2>/dev/null || echo "  (none found)"
fi

echo ""
echo "=== Test Result ==="

if [[ "${COREDUMP_FOUND}" -eq 0 ]]; then
    echo "FAILED: No coredump file was found."
    echo ""
    echo "Possible reasons:"
    echo "  1. Core dumps may be disabled on the rbe host (check 'ulimit -c')"
    echo "  2. The core pattern may not include .core suffix"
    echo "  3. The test may not have run with the ci_wrapper"
    exit 1
fi

echo "Coredump file found: ${COREDUMP_FILE}"

# Show file info
if [[ -f "${COREDUMP_FILE}" ]]; then
    FILE_SIZE=$(stat -c%s "${COREDUMP_FILE}" 2>/dev/null || stat -f%z "${COREDUMP_FILE}" 2>/dev/null)
    echo "File size: ${FILE_SIZE} bytes"
fi

echo ""
echo "=== Verifying Coredump with GDB ==="
echo ""

# Prepare the coredump for gdb (unzip if needed)
TEMP_DIR=$(mktemp -d)

if [[ "${COREDUMP_FILE}" == *.gz ]]; then
    echo "Decompressing coredump..."
    UNZIPPED_CORE="${TEMP_DIR}/core"
    gunzip -c "${COREDUMP_FILE}" >"${UNZIPPED_CORE}"
    CORE_FOR_GDB="${UNZIPPED_CORE}"
else
    CORE_FOR_GDB="${COREDUMP_FILE}"
fi

# The test binary path
TEST_BINARY="bazel-bin/buildscripts/bazel_testbuilds/ci_wrapper_timeout_test"

echo "Running gdb to verify coredump source..."
echo "Binary: ${TEST_BINARY}"
echo "Core: ${CORE_FOR_GDB}"
echo ""

# Run gdb in batch mode and capture the "Core was generated by" line
GDB_OUTPUT=$(gdb "${TEST_BINARY}" "${CORE_FOR_GDB}" -batch -ex "quit" 2>&1)
GDB_EXIT_CODE=$?

# Extract the "Core was generated by" line
CORE_GENERATED_LINE=$(echo "${GDB_OUTPUT}" | grep -i "Core was generated by")

echo "GDB output (Core was generated by line):"
echo "  ${CORE_GENERATED_LINE}"
echo ""

if [[ -z "${CORE_GENERATED_LINE}" ]]; then
    echo "WARNING: Could not find 'Core was generated by' line in gdb output."
    echo "Full gdb output:"
    echo "${GDB_OUTPUT}"
    echo ""
    echo "FAILED: Unable to verify coredump source."
    exit 1
fi

# Verify the coredump was generated by the actual test binary, not by /bin/bash
# Good: Core was generated by `.../ci_wrapper_timeout_test'
# Bad:  Core was generated by `/bin/bash .../ci_wrapper_timeout_test'

if echo "${CORE_GENERATED_LINE}" | grep -q "/bin/bash"; then
    echo "FAILED: Coredump was generated by /bin/bash (the wrapper script)."
    echo "This indicates the coredump is from the shell wrapper, not the actual test binary."
    echo ""
    echo "Expected: Core was generated by '...ci_wrapper_timeout_test'"
    echo "Got: ${CORE_GENERATED_LINE}"
    exit 1
fi

# Verify it contains the expected test binary name
if echo "${CORE_GENERATED_LINE}" | grep -q "ci_wrapper_timeout_test"; then
    echo "SUCCESS: Coredump was generated by the correct test binary."
    echo ""
    echo "=== Test Result ==="
    echo "PASSED: Coredump was successfully generated by the test binary (not the wrapper script)."
    exit 0
else
    echo "WARNING: Coredump source doesn't match expected test binary name."
    echo "Expected to find 'ci_wrapper_timeout_test' in the path."
    echo "Got: ${CORE_GENERATED_LINE}"
    echo ""
    echo "FAILED: Coredump source verification failed."
    exit 1
fi
