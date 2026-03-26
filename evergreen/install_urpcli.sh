#!/bin/bash
# install_urpcli.sh — bootstrap urpcli binary for Evergreen tasks
#
# Usage: source this script or run it directly before any urpcli calls.
# Skips download if urpcli is already present, or if SKIP_URPCLI_INSTALL=true
# (set that env var when urpcli is provided by the URP Evergreen module).

set -o errexit
set -o pipefail

URPCLI_INSTALL_DIR="${workdir:-$HOME}/bin"
URPCLI_BIN="${URPCLI_INSTALL_DIR}/urpcli"

if [[ "${SKIP_URPCLI_INSTALL:-false}" == "true" ]]; then
    echo "SKIP_URPCLI_INSTALL=true — skipping urpcli download"
    return 0 2>/dev/null || exit 0
fi

if [[ -f "${URPCLI_BIN}" ]]; then
    echo "urpcli already present at ${URPCLI_BIN} — skipping download"
    return 0 2>/dev/null || exit 0
fi

# Determine platform + arch for the download URL
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    _urpcli_os="linux"
elif [[ "$OSTYPE" == "darwin"* ]]; then
    _urpcli_os="darwin"
else
    echo "install_urpcli.sh: unsupported OS: ${OSTYPE}" >&2
    exit 1
fi

_urpcli_arch=$(uname -m)
case "${_urpcli_arch}" in
    x86_64)  _urpcli_arch_tag="x86_64" ;;
    aarch64) _urpcli_arch_tag="arm64" ;;
    arm64)   _urpcli_arch_tag="arm64" ;;
    *)
        echo "install_urpcli.sh: unsupported architecture: ${_urpcli_arch}" >&2
        exit 1
        ;;
esac

URPCLI_URL="https://s3.amazonaws.com/urp-releases-prod/urpcli/latest/urpcli-${_urpcli_os}-${_urpcli_arch_tag}"

echo "Downloading urpcli from ${URPCLI_URL} to ${URPCLI_BIN}"
mkdir -p "${URPCLI_INSTALL_DIR}"
curl --retry 3 --retry-delay 5 --fail --silent --show-error -o "${URPCLI_BIN}" "${URPCLI_URL}"
chmod +x "${URPCLI_BIN}"

# Make sure it's on PATH for the rest of this task
export PATH="${URPCLI_INSTALL_DIR}:${PATH}"
echo "urpcli installed: $(${URPCLI_BIN} --version 2>/dev/null || echo '(version unavailable)')"
