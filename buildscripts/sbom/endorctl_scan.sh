#!/bin/bash
# Run an endorctl scan. All scan flags are driven by ENDORCTL_* environment variables.
# See: https://docs.endorlabs.com/developers-api/cli/environment-variables
#      https://docs.endorlabs.com/developers-api/cli/commands/scan#options

set -o errexit
set -o xtrace

# Optional: set up a Rust toolchain when the scan requires cargo.
# Set RUST_SETUP_DIR to the path of a directory containing .evergreen/install-dependencies.sh
# (e.g. the monguard directory). The github_token env var must also be set for private
# 10gen dependency access.
if [ -n "${RUST_SETUP_DIR}" ]; then
    pushd "${RUST_SETUP_DIR}"
    export PROJECT_DIRECTORY="${PWD}"
    source .evergreen/install-dependencies.sh rust
    source .evergreen/env.sh
    # env.sh sets CARGO_HOME inside the repo checkout (src/.cargo); override it to sit outside
    # the repo root so endorctl does not scan the cargo package cache.
    export CARGO_HOME="$(dirname "$(dirname "${RUST_SETUP_DIR}")")/.cargo"
    source .evergreen/cargo-auth-env.sh
    popd
    cd "$(dirname "${RUST_SETUP_DIR}")"
fi

"${ENDORCTL_PATH:-endorctl}" scan
