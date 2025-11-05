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

    # Add CLI to PATH (installed locally by cli_setup.sh)
    export PATH="$PWD/.devcontainer-cli/bin:$PATH"
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

# Run commands inside the container using devcontainer exec
# Pass the same --id-label so it can find the container we created
echo "Checking GCC version..."
devcontainer exec --workspace-folder . --id-label "test-id=${TEST_ID}" gcc --version

echo ""
echo "Checking Python version..."
devcontainer exec --workspace-folder . --id-label "test-id=${TEST_ID}" python3 --version

echo ""
echo "Checking Python venv..."
devcontainer exec --workspace-folder . --id-label "test-id=${TEST_ID}" bash -c "source python3-venv/bin/activate && python --version"

echo ""
echo "Checking Bazel..."
devcontainer exec --workspace-folder . --id-label "test-id=${TEST_ID}" bazel --version

echo ""
echo "Checking Git..."
devcontainer exec --workspace-folder . --id-label "test-id=${TEST_ID}" git --version

echo ""
echo "Checking clangd configuration..."
if ! devcontainer exec --workspace-folder . --id-label "test-id=${TEST_ID}" test -f compile_commands.json; then
    echo "ERROR: compile_commands.json not found - clangd setup failed"
    exit 1
fi
echo "✓ compile_commands.json exists"

echo ""
echo "Checking .clang-tidy configuration..."
if ! devcontainer exec --workspace-folder . --id-label "test-id=${TEST_ID}" test -f .clang-tidy; then
    echo "ERROR: .clang-tidy not found - clang-tidy setup failed"
    exit 1
fi
echo "✓ .clang-tidy exists"

echo ""
echo "=== Stopping devcontainer ==="

# Stop and remove the container
docker stop "$CONTAINER_ID"
docker rm "$CONTAINER_ID"

echo ""
echo "✅ Fresh setup test PASSED"
