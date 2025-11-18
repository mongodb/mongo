#!/bin/bash
# Test full devcontainer setup using devcontainer CLI
# This simulates the complete user experience

set -euo pipefail

echo "========================================"
echo "Devcontainer Fresh Setup Test"
echo "========================================"

# Get the absolute path to the repo root (src directory)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# Create temp directory for devcontainer CLI installation
CLI_INSTALL_DIR=$(mktemp -d)
export CLI_INSTALL_DIR

# Set up cleanup trap to remove temp directory on exit
cleanup() {
    if [ -d "$CLI_INSTALL_DIR" ]; then
        echo "Cleaning up devcontainer CLI installation..."
        rm -rf "$CLI_INSTALL_DIR"
    fi
}
trap cleanup EXIT

# Set a non-conflicting username for CI
# In CI, USER is often "ubuntu" which conflicts with system users
# Use a distinct name that won't conflict
if [ "${USER:-}" = "ubuntu" ] || [ "${USER:-}" = "root" ]; then
    export USER="mongociuser"
    echo "CI environment detected, using USER=$USER"
fi

# Configure engflow credentials if they were fetched by evergreen
echo ""
echo "=== Configuring engflow credentials ==="
if [ -f "${REPO_ROOT}/engflow.key" ] && [ -f "${REPO_ROOT}/engflow.cert" ]; then
    echo "✓ Engflow credentials found, configuring for devcontainer"
    echo "common --tls_client_certificate=./engflow.cert" >>.bazelrc.evergreen
    echo "common --tls_client_key=./engflow.key" >>.bazelrc.evergreen
else
    echo "Info: No engflow credentials found (local execution will be used)"
fi

# Ensure devcontainer CLI is available
if ! command -v devcontainer &>/dev/null; then
    echo "Installing devcontainer CLI..."
    bash "$SCRIPT_DIR/devcontainer_cli_setup.sh"

    # Add CLI to PATH (installed by cli_setup.sh)
    export PATH="${CLI_INSTALL_DIR}/bin:$PATH"
fi

echo "Using devcontainer CLI version:"
devcontainer --version

echo ""
echo "=== Building and starting devcontainer ==="

# Use a unique test ID to reliably find the container later
TEST_ID="mongo-devcontainer-test-$$"

# Set CI environment variable to enable strict error handling in the container
export CI=true

# Use devcontainer CLI to build and start (same as VS Code does)
# Use --id-label to tag the container so we can find it reliably
# Pass CI=true to enable strict error handling in the container
devcontainer up --workspace-folder . --id-label "test-id=${TEST_ID}" --update-remote-user-uid-default "off" --remote-env CI=true

echo ""
echo "=== Finding container ==="

# Find the container using our custom label
CONTAINER_ID=$(docker ps -a --filter "label=test-id=${TEST_ID}" --format "{{.ID}}" | head -1)

if [ -z "$CONTAINER_ID" ]; then
    echo "ERROR: Failed to find devcontainer with label test-id=${TEST_ID}"
    echo "Running containers:"
    docker ps -a
    exit 1
fi

echo "Container ID: $CONTAINER_ID"

echo ""
echo "=== Testing inside devcontainer ==="

# Helper function to run commands inside the container
# Pass CI=true to all exec commands so engflow_auth wrapper skips credential helper setup
# Pass the same --id-label so it can find the container we created
devcontainer_run() {
    devcontainer exec --workspace-folder . --id-label "test-id=${TEST_ID}" --remote-env CI=true "$@"
}

echo "Checking core dump pattern configuration..."
CORE_PATTERN=$(devcontainer_run cat /proc/sys/kernel/core_pattern)
if [[ "$CORE_PATTERN" == "dump_%e.%p.core" ]]; then
    echo "✓ Core dump pattern correctly set to: $CORE_PATTERN"
else
    echo "✗ Core dump pattern is: $CORE_PATTERN (expected: dump_%e.%p.core)"
    echo "  The initializeCommand may have failed to set the kernel parameter"
    exit 1
fi

echo ""
echo "Checking GCC version..."
devcontainer_run gcc --version

echo ""
echo "Checking Python version..."
devcontainer_run python3 --version

echo ""
echo "Checking Python venv..."
devcontainer_run bash -c "source python3-venv/bin/activate && python --version"

echo ""
echo "Checking Bazel..."
devcontainer_run bazel --version

echo ""
echo "Checking Git..."
devcontainer_run git --version

echo ""
echo "Checking Evergreen CLI..."
devcontainer_run evergreen --version

echo ""
echo "Checking clangd configuration..."
if ! devcontainer_run test -f compile_commands.json; then
    echo "ERROR: compile_commands.json not found - clangd setup failed"
    exit 1
fi
echo "✓ compile_commands.json exists"

echo ""
echo "Checking .clang-tidy configuration..."
if ! devcontainer_run test -f .clang-tidy; then
    echo "ERROR: .clang-tidy not found - clang-tidy setup failed"
    exit 1
fi
echo "✓ .clang-tidy exists"

echo ""
echo "=== Running representative user operations ==="

echo ""
echo "Running C++ unit test..."
devcontainer_run bazel test //src/mongo/bson:bson_test --config=local --test_output=errors

echo ""
echo "Building IDL target (tests code generation)..."
devcontainer_run bazel build //src/mongo/bson:bson_validate --config=local

echo ""
echo "Building install-dist-test target (necessary for resmoke below)..."
devcontainer_run bazel build install-dist-test

echo ""
echo "Running Python test via resmoke..."
devcontainer_run python3 buildscripts/resmoke.py run --suite=core --sanityCheck

echo ""
echo "Checking code formatting..."
devcontainer_run bazel run format

echo ""
echo "Running linter..."
devcontainer_run bazel run lint

echo ""
echo "✅ All representative operations passed"

echo ""
echo "=== Stopping devcontainer ==="

# Stop and remove the container
docker stop "$CONTAINER_ID"
docker rm "$CONTAINER_ID"

echo ""
echo "✅ Fresh setup test PASSED"
