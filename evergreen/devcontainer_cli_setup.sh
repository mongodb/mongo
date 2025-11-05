#!/bin/bash
# Install devcontainer CLI for testing
# This script sets up the devcontainer CLI tool used by VS Code

set -euo pipefail

echo "========================================"
echo "Installing Devcontainer CLI"
echo "========================================"

# Verify Node.js is available
if ! command -v node &>/dev/null; then
    echo "ERROR: Node.js not found (should be pre-installed on distro)"
    exit 1
fi

echo "Node.js version:"
node --version

# Verify npm is available
if ! command -v npm &>/dev/null; then
    echo "ERROR: npm not found (should come with Node.js)"
    exit 1
fi

echo "npm version:"
npm --version

# Install devcontainer CLI locally in task workdir
echo ""
echo "Installing @devcontainers/cli..."
INSTALL_DIR="$PWD/.devcontainer-cli"
mkdir -p "$INSTALL_DIR"
npm install -g --prefix "$INSTALL_DIR" @devcontainers/cli

# Add to PATH for this script and subsequent commands
export PATH="$INSTALL_DIR/bin:$PATH"

# Verify installation
echo ""
echo "Verifying devcontainer CLI installation..."
devcontainer --version
echo "Installed to: $INSTALL_DIR"

echo ""
echo "✅ Devcontainer CLI installed successfully"
