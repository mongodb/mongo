#!/bin/bash
set -euo pipefail

# MongoDB Development Container Post-Create Setup Script
# This script handles setup tasks that run when the container is first created,
# including infrastructure setup, developer tooling installation, and workspace
# configuration. Steps are idempotent and safe to re-run.

echo "Starting MongoDB development container post-create setup..."

# Setup user's .bazelrc to import system-wide devcontainer config
echo "Setting up Bazel configuration..."
if [ -f "/etc/devcontainer/bazelrc" ]; then
    if [ ! -f "${HOME}/.bazelrc" ]; then
        # Create new .bazelrc with import
        echo "# Import devcontainer system configuration" >"${HOME}/.bazelrc"
        echo "try-import /etc/devcontainer/bazelrc" >>"${HOME}/.bazelrc"
        echo "[OK] Created .bazelrc with system config import"
    elif ! grep -q "try-import /etc/devcontainer/bazelrc" "${HOME}/.bazelrc" 2>/dev/null; then
        # Add import to existing .bazelrc
        echo "" >>"${HOME}/.bazelrc"
        echo "# Import devcontainer system configuration" >>"${HOME}/.bazelrc"
        echo "try-import /etc/devcontainer/bazelrc" >>"${HOME}/.bazelrc"
        echo "[OK] Added system config import to existing .bazelrc"
    else
        echo "Info: .bazelrc already imports system config"
    fi
fi

# Fix volume permissions
echo "Fixing volume permissions..."
sudo chown -R "$(whoami)": "${HOME}" || echo "Warning: Could not fix home directory permissions"
sudo chown -R "$(whoami)": "${WORKSPACE_FOLDER}/python3-venv" || echo "Warning: Could not fix python3-venv permissions"
sudo chown "$(whoami)": "${WORKSPACE_FOLDER}/.." || echo "Warning: Could not fix parent directory permissions"

# Fix workspace root permissions (prevents bazelrc and other file permission issues)
echo "Fixing workspace root permissions..."
sudo chown -R "$(whoami)": "${WORKSPACE_FOLDER}" || echo "Warning: Could not fix workspace permissions"

# Fix Git repository permissions (prevents "insufficient permission" errors)
echo "Fixing Git repository permissions..."
sudo chown -R "$(whoami)": "${WORKSPACE_FOLDER}/.git" || echo "Warning: Could not fix .git permissions"

# Configure git safe.directory (prevents "dubious ownership" warnings)
echo "Configuring git safe.directory..."
git config --global --add safe.directory "${WORKSPACE_FOLDER}" || echo "Warning: Could not configure git safe.directory"
echo "[OK] Volume and Git permissions fixed"

# Configure Bazel with Docker information (one-time container setup)
echo "Configuring Bazel with Docker server information..."

# Helper function to add/update Bazel keyword with latest value
add_bazel_keyword() {
    local keyword="$1"
    local value="$2"
    local bazelrc="${HOME}/.bazelrc"

    # Ensure bazelrc exists
    touch "${bazelrc}"

    # Use a temporary file to avoid issues with in-place editing
    grep -v "devcontainer:${keyword}" "${bazelrc}" >"${bazelrc}.tmp" 2>/dev/null || true
    mv "${bazelrc}.tmp" "${bazelrc}"

    # Add the keyword with current value
    echo "common --bes_keywords=devcontainer:${keyword}=\"${value}\"" >>"${bazelrc}"
    echo "[OK] ${keyword} configured: ${value}"
}

# Report Docker server platform and version
OS_NAME=$(docker info --format '{{.OperatingSystem}}' 2>/dev/null || echo "unknown")
DOCKER_NAME=$(docker info --format '{{.Name}}' 2>/dev/null || echo "unknown")
DOCKER_VERSION=$(docker info --format '{{.ServerVersion}}' 2>/dev/null || echo "unknown")

# Determine platform
if [ "${OS_NAME}" = "Docker Desktop" ]; then
    DOCKER_PLATFORM="Docker Desktop"
elif [[ "${DOCKER_NAME}" =~ rancher ]]; then
    DOCKER_PLATFORM="Rancher Desktop"
elif [ "${OS_NAME}" != "unknown" ]; then
    DOCKER_PLATFORM="${OS_NAME}"
else
    DOCKER_PLATFORM="unknown"
fi

add_bazel_keyword "docker_server_platform" "${DOCKER_PLATFORM}"
add_bazel_keyword "docker_server_version" "${DOCKER_VERSION}"

# Report architecture
ARCH=$(uname -i 2>/dev/null || echo "unknown")
add_bazel_keyword "arch" "${ARCH}"

# Unshallow Git repository (one-time setup)
echo "Setting up Git repository..."

# Helper function to handle git shallow lock
wait_for_git_lock() {
    local lockfile=".git/shallow.lock"
    if [ -f "${lockfile}" ]; then
        echo "Warning: Git shallow lock detected, waiting for it to clear..."
        local count=0
        while [ -f "${lockfile}" ] && [ $count -lt 30 ]; do
            sleep 1
            ((count++))
        done

        # If lock still exists after 30 seconds, remove it
        if [ -f "${lockfile}" ]; then
            echo "Warning: Removing stale git shallow lock..."
            rm -f "${lockfile}"
        fi
    fi
}

wait_for_git_lock

# Proceed with git operations
if [ -f .git/shallow ]; then
    echo "Info: Detected shallow repository, fetching complete history with tags..."
    if git fetch --unshallow --tags; then
        echo "[OK] Git repository unshallowed and tags fetched successfully"
    else
        echo "Warning: Failed to unshallow repository and fetch tags"
        echo "Hint: You can manually run: git fetch --unshallow --tags"
    fi
else
    echo "Info: Repository is already complete, fetching tags..."
    if git fetch --tags; then
        echo "[OK] Git tags fetched successfully"
    else
        echo "Warning: Failed to fetch tags"
        echo "Hint: You can manually run: git fetch --tags"
    fi
fi

# Create Python virtual environment and install dependencies
echo "Creating Python virtual environment..."
venv_created=false
if [ ! -d "${WORKSPACE_FOLDER}/python3-venv/bin" ]; then
    export PYTHON_KEYRING_BACKEND=keyring.backends.null.Keyring
    /opt/mongodbtoolchain/v5/bin/python3 -m venv "${WORKSPACE_FOLDER}/python3-venv"
    venv_created=true

    # Install dependencies in the newly created venv
    set +o nounset
    source "${WORKSPACE_FOLDER}/python3-venv/bin/activate"
    set -o nounset

    if command -v poetry &>/dev/null; then
        POETRY_VIRTUALENVS_IN_PROJECT=true poetry install --no-root --sync
        echo "[OK] Python virtual environment created and dependencies installed"
    else
        echo "Warning: poetry not available, skipping dependency installation"
    fi

    set +o nounset
    deactivate
    set -o nounset
else
    echo "Info: Python virtual environment already exists"
fi

# Build clang configuration
echo "Building clang configuration..."

# Backup existing .bazelrc.compiledb if it exists, then use our known-good config
COMPILEDB_RC="${WORKSPACE_FOLDER}/.bazelrc.compiledb"
COMPILEDB_RC_BACKUP="${WORKSPACE_FOLDER}/.bazelrc.compiledb.backup"

if [ -f "${COMPILEDB_RC}" ]; then
    echo "Info: Backing up existing .bazelrc.compiledb"
    mv "${COMPILEDB_RC}" "${COMPILEDB_RC_BACKUP}"
fi

# Create our temporary config for setup (allow remote execution if auth is configured)
echo "common --config=dbg" >"${COMPILEDB_RC}"

# Check for engflow credentials (supports three auth methods: workspace certs, volume certs, or browser token)
if [ -f "${WORKSPACE_FOLDER}/engflow.cert" ] && [ -f "${WORKSPACE_FOLDER}/engflow.key" ]; then
    echo "Info: Engflow TLS credentials found in workspace (CI mode), using remote execution"
elif [ -d "${HOME}/.config/engflow_auth" ] && [ -n "$(ls -A ${HOME}/.config/engflow_auth 2>/dev/null)" ]; then
    echo "Info: Engflow auth directory exists (likely browser token), using remote execution"
else
    echo "Info: No engflow credentials found, using local execution"
    echo "common --config=local" >>"${COMPILEDB_RC}"
fi

# Ensure restore happens even if build fails
restore_compiledb_rc() {
    if [ -f "${COMPILEDB_RC_BACKUP}" ]; then
        echo "Info: Restoring user's .bazelrc.compiledb"
        mv "${COMPILEDB_RC_BACKUP}" "${COMPILEDB_RC}"
    else
        rm -f "${COMPILEDB_RC}"
    fi
}
trap restore_compiledb_rc EXIT

if bazel build compiledb; then
    echo "[OK] Clang configuration built successfully"
else
    echo "Warning: Failed to build clang configuration"
fi

# Setup GDB pretty printers
echo "Setting up GDB pretty printers..."
cd "${WORKSPACE_FOLDER}/.."
if [ -d "Boost-Pretty-Printer" ]; then
    echo "Info: Boost-Pretty-Printer already exists"
else
    if git clone https://github.com/mongodb-forks/Boost-Pretty-Printer.git; then
        if ! grep -q "source ${HOME}/gdbinit" "${HOME}/.gdbinit" 2>/dev/null; then
            echo "" >>"${HOME}/.gdbinit"
            echo "# BEGIN Server Workflow Tool gdbinit" >>"${HOME}/.gdbinit"
            echo "source ${HOME}/gdbinit" >>"${HOME}/.gdbinit"
            echo "# END Server Workflow Tool gdbinit" >>"${HOME}/.gdbinit"
        fi
        echo "[OK] GDB pretty printers installed"
    else
        echo "Warning: Failed to clone Boost-Pretty-Printer"
    fi
fi
cd "${WORKSPACE_FOLDER}"

# Configure automatic Python virtual environment activation
echo "Configuring automatic Python virtual environment activation..."

# Helper function to add Python venv activation to shell config
configure_shell_venv() {
    local shell_config="$1"
    local shell_name="$2"

    if [ -f "${shell_config}" ]; then
        if ! grep -q "python3-venv/bin/activate" "${shell_config}" 2>/dev/null; then
            cat >>"${shell_config}" <<EOF

# Auto-activate MongoDB Python virtual environment
if [ -f "${WORKSPACE_FOLDER}/python3-venv/bin/activate" ]; then
    source "${WORKSPACE_FOLDER}/python3-venv/bin/activate"
fi
EOF
            echo "[OK] Python venv activation added to ${shell_name}"
        else
            echo "Info: Python venv activation already configured in ${shell_name}"
        fi
    fi
}

if [ -f "${WORKSPACE_FOLDER}/python3-venv/bin/activate" ]; then
    configure_shell_venv "${HOME}/.bashrc" ".bashrc"
    configure_shell_venv "${HOME}/.zshrc" ".zshrc"
else
    echo "Warning: Python virtual environment not found at ${WORKSPACE_FOLDER}/python3-venv"
fi

# Sync Python dependencies
echo "Syncing Python dependencies..."
if [ -f "${WORKSPACE_FOLDER}/python3-venv/bin/activate" ]; then
    # Only run sync if venv was just created OR if we're updating an existing one
    if [ "${venv_created}" = true ]; then
        echo "Info: Skipping sync for newly created venv (already installed via poetry)"
    else
        source "${WORKSPACE_FOLDER}/python3-venv/bin/activate"

        if "${WORKSPACE_FOLDER}/buildscripts/poetry_sync.sh"; then
            echo "[OK] Python dependencies synced successfully"
        else
            echo "Warning: Failed to sync Python dependencies"
        fi

        deactivate
    fi
else
    echo "Warning: Python virtual environment not found at ${WORKSPACE_FOLDER}/python3-venv"
fi

# Install Node modules
echo "Installing Node.js dependencies..."
if bazel run @pnpm//:pnpm --config=local -- --dir "${WORKSPACE_FOLDER}" install; then
    echo "[OK] Node.js dependencies installed successfully"
else
    echo "Error: Failed to install Node.js dependencies"
    exit 1
fi

echo ""
echo "MongoDB development container setup completed successfully!"
echo "Info: All infrastructure and dependencies are now configured and up to date."
echo "Info: VS Code Python extension automatically handles virtual environment activation."
echo "Info: You can now start developing with MongoDB!"
