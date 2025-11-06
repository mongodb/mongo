#!/bin/bash

# Build JS Engine Shell Script
# Converted from Docker commands to standalone shell script

set -euo pipefail # Exit on error, undefined variables, and pipe failures

# Default Node.js version
NODE_VERSION="${NODE_VERSION:-16}"

# Function to show usage
usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Build and setup JS Engine with Node.js

OPTIONS:
    -n, --node-version VERSION    Node.js major version to install (default: 16)
    -b, --build-dir DIR           Build directory (JS engine source path)
    -t, --target-dir DIR          Target directory to copy JS engine files to
    -h, --help                    Show this help message

EXAMPLES:
    $0                                              # Install Node.js 16.x (default)
    $0 -n 18                                        # Install Node.js 18.x
    $0 --node-version 20 -t /app                    # Install Node.js 20.x and copy files to /app
    $0 -b /path/to/source -t /app                   # Use custom build dir and copy to /app
    $0 -n 18 -b ./js-engine -t /opt/app             # Full example with all options

ENVIRONMENT VARIABLES:
    NODE_VERSION            Default Node.js version if -n not specified (default: 16)

NOTES:
    - Build directory (-b) is required when building/copying the JS engine
    - Target directory (-t) is optional; if not specified, files won't be copied
    - The script will verify that package.json exists in the build directory

EOF
    exit 0
}

# Function to log messages
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

# Function to check if running as root (needed for package installation)
check_root() {
    if [[ $EUID -ne 0 ]]; then
        echo "Error: This script must be run as root for package installation"
        echo "Please run with sudo: sudo $0"
        exit 1
    fi
}

# Function to detect package manager
detect_package_manager() {
    if command -v yum >/dev/null 2>&1; then
        echo "yum"
    elif command -v apt-get >/dev/null 2>&1; then
        echo "apt"
    elif command -v dnf >/dev/null 2>&1; then
        echo "dnf"
    else
        echo "unknown"
    fi
}

# Main installation function
# Add NodeSource repository via https://nodesource.com/products/distributions
install_nodejs() {
    local pkg_manager
    pkg_manager=$(detect_package_manager)

    log "Detected package manager: $pkg_manager"
    log "Installing Node.js version: ${NODE_VERSION}.x"

    case $pkg_manager in
    "yum")
        log "Adding NodeSource repository via https://nodesource.com/products/distributions"
        wget -qO- "https://rpm.nodesource.com/setup_${NODE_VERSION}.x" | bash -

        log "Installing Node.js (npm is included)"
        yum install -y nodejs

        log "Cleaning up"
        yum clean all
        ;;
    "dnf")
        log "Adding NodeSource repository via https://nodesource.com/products/distributions"
        wget -qO- "https://rpm.nodesource.com/setup_${NODE_VERSION}.x" | bash -

        log "Installing Node.js (npm is included)"
        dnf install -y nodejs

        log "Cleaning up"
        dnf clean all
        ;;
    "apt")
        log "Adding NodeSource repository via https://nodesource.com/products/distributions"
        wget -qO- "https://deb.nodesource.com/setup_${NODE_VERSION}.x" | bash -

        log "Installing Node.js (npm is included)"
        apt-get install -y nodejs

        log "Cleaning up"
        apt-get clean
        ;;
    *)
        log "Error: Unsupported package manager. Please install Node.js manually."
        exit 1
        ;;
    esac
}

# Function to verify Node.js installation
verify_nodejs() {
    log "Verifying Node.js installation"

    if ! command -v node >/dev/null 2>&1; then
        log "Error: node command not found"
        exit 1
    fi

    if ! command -v npm >/dev/null 2>&1; then
        log "Error: npm command not found"
        exit 1
    fi

    local node_version
    local npm_version
    node_version=$(node -v)
    npm_version=$(npm -v)

    log "Node.js version: $node_version"
    log "npm version: $npm_version"
}

# Function to verify build directory
verify_build_dir() {
    local build_dir="$1"

    log "Verifying build directory: $build_dir"

    if [[ -z "$build_dir" ]]; then
        log "Error: Build directory is empty or unset"
        log "Please specify with -b/--build-dir argument"
        return 1
    fi

    if [[ ! -d "$build_dir" ]]; then
        log "Error: Build directory does not exist: $build_dir"
        return 1
    fi

    if [[ ! -f "$build_dir/package.json" ]]; then
        log "Error: No package.json found in $build_dir"
        return 1
    fi

    log "✓ Build directory verified: $build_dir"
    return 0
}

# Function to build JS engine production output
build_js_engine() {
    local build_dir="$1"

    log "Building JS engine production output in $build_dir"

    # Change to build directory
    pushd "$build_dir" >/dev/null

    # Install dependencies
    log "Running npm ci to install dependencies..."
    npm ci --legacy-peer-deps

    # Build production output
    log "Running npm run build to create production output..."
    npm run build

    # Verify dist directory was created
    if [[ ! -d "dist" ]]; then
        log "Error: dist directory not found after build"
        popd >/dev/null
        exit 1
    fi

    log "✓ JS engine built successfully"

    # Return to original directory
    popd >/dev/null
}

# Function to copy JS engine production output
copy_js_engine() {
    local build_dir="$1"
    local target_dir="$2"

    log "Copying JS engine production output from $build_dir/dist to $target_dir"

    # Create target directory if it doesn't exist
    mkdir -p "$target_dir"

    # Copy only the dist folder contents with proper permissions
    cp -r "$build_dir/dist"/* "$target_dir/"
    chmod -R 755 "$target_dir"

    log "✓ JS engine production output copied successfully"
}

# Parse command line arguments
parse_args() {
    local build_dir=""
    local target_dir=""
    local node_version="$NODE_VERSION" # Start with default/global value

    while [[ $# -gt 0 ]]; do
        case $1 in
        -n | --node-version)
            if [[ -z "${2:-}" ]] || [[ "$2" =~ ^- ]]; then
                echo "Error: --node-version requires a version number"
                exit 1
            fi
            node_version="$2"
            echo "DEBUG: Setting NODE_VERSION to $node_version" >&2
            shift 2
            ;;
        -b | --build-dir)
            if [[ -z "${2:-}" ]] || [[ "$2" =~ ^- ]]; then
                echo "Error: --build-dir requires a directory path"
                exit 1
            fi
            build_dir="$2"
            shift 2
            ;;
        -t | --target-dir)
            if [[ -z "${2:-}" ]] || [[ "$2" =~ ^- ]]; then
                echo "Error: --target-dir requires a directory path"
                exit 1
            fi
            target_dir="$2"
            shift 2
            ;;
        -h | --help)
            usage
            ;;
        -*)
            echo "Error: Unknown option: $1"
            usage
            ;;
        *)
            echo "Error: Unexpected argument: $1"
            echo "Use -t/--target-dir for target directory and -b/--build-dir for build directory"
            usage
            ;;
        esac
    done

    # Return all three values separated by delimiters
    echo "${node_version}|${build_dir}|${target_dir}"
}

# Main execution
main() {
    log "Starting JS Engine build script"
    log "Arguments received: $*"
    log "NODE_VERSION before parsing: $NODE_VERSION"

    # Parse arguments
    local args_result
    local build_dir
    local target_dir
    args_result=$(parse_args "$@")

    # Extract values: node_version|build_dir|target_dir
    NODE_VERSION="${args_result%%|*}"
    local remaining="${args_result#*|}"
    build_dir="${remaining%%|*}"
    target_dir="${remaining#*|}"

    log "NODE_VERSION after parsing: $NODE_VERSION"
    log "Build directory: $build_dir"
    log "Target directory: $target_dir"

    # Check if we need to install Node.js (skip if already installed and correct version)
    if command -v node >/dev/null 2>&1; then
        local current_version
        local major_version
        current_version=$(node -v | sed 's/v//')
        major_version=$(echo "$current_version" | cut -d. -f1)

        if [[ "$major_version" == "$NODE_VERSION" ]]; then
            log "Node.js ${NODE_VERSION}.x already installed: v$current_version"
        else
            log "Different Node.js version detected: v$current_version. Installing Node.js ${NODE_VERSION}.x..."
            check_root
            install_nodejs
        fi
    else
        log "Node.js not found. Installing Node.js ${NODE_VERSION}.x..."
        check_root
        install_nodejs
    fi

    verify_nodejs

    log "Verifying build directory: $build_dir"

    # Verify the build directory
    if ! verify_build_dir "$build_dir"; then
        log "Build directory verification failed"
        exit 1
    fi

    log "Verified build directory"

    # Build the JS engine production output
    build_js_engine "$build_dir"

    # Copy JS engine production output if target directory is specified
    if [[ -n "$target_dir" ]]; then
        copy_js_engine "$build_dir" "$target_dir"
    else
        log "No target directory specified. Skipping file copy."
        log "Production output is available in: $build_dir/dist"
        log "To copy files, use: $0 -t <target_directory>"
    fi

    log "JS Engine build script completed successfully"
}

# Run main function with all arguments
main "$@"
