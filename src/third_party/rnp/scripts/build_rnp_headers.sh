#!/bin/bash
# Build script for RNP generated headers (config.h, version.h)
#
# RNP uses CMake and generates headers that are NOT platform-specific within Linux.
# This script runs CMake configure in a manylinux2014 container to generate
# the required headers reproducibly.
#
# Usage:
#   ./build_rnp_headers.sh
#   VERBOSE=1 ./build_rnp_headers.sh
#   CRYPTO_BACKEND=openssl ./build_rnp_headers.sh   # Use OpenSSL instead of Botan

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RNP_SRC="$SCRIPT_DIR/../dist"
OUT_DIR="$SCRIPT_DIR/../dist/build"

# Configuration
VERBOSE="${VERBOSE:-0}"
PULL="${PULL:-1}"
CRYPTO_BACKEND="${CRYPTO_BACKEND:-openssl}"  # openssl or botan

# Use native architecture - headers are not platform-specific
# Using manylinux_2_28 (AlmaLinux 8) which has OpenSSL 1.1.1+ required by RNP
ARCH=$(uname -m)
case "$ARCH" in
  x86_64)  DOCKER_IMAGE="quay.io/pypa/manylinux_2_28_x86_64" ;;
  aarch64) DOCKER_IMAGE="quay.io/pypa/manylinux_2_28_aarch64" ;;
  *)       echo "Unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

# Check source exists
if [ ! -d "$RNP_SRC" ]; then
  echo "ERROR: RNP source not found at $RNP_SRC" >&2
  echo "Run import.sh first." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

echo "==> RNP header generator"
echo "    Source: $RNP_SRC"
echo "    Output: $OUT_DIR"
echo "    Crypto backend: $CRYPTO_BACKEND"
echo "    VERBOSE: $VERBOSE"

if [ "$PULL" = "1" ]; then
  echo "==> Pulling image: $DOCKER_IMAGE"
  docker pull "$DOCKER_IMAGE"
fi

docker run --rm -t \
    -e VERBOSE="$VERBOSE" \
    -e CRYPTO_BACKEND="$CRYPTO_BACKEND" \
    -e HOST_UID="$(id -u)" \
    -e HOST_GID="$(id -g)" \
    -v "$RNP_SRC":/src/rnp:ro \
    -v "$OUT_DIR":/out \
    "$DOCKER_IMAGE" \
    bash -lc '
      set -euo pipefail

      # Install build tools and dependencies
      # manylinux_2_28 (AlmaLinux 8) has OpenSSL 1.1.1+ and uses dnf
      if [ "${VERBOSE:-0}" = "1" ]; then
        dnf -y install make gcc gcc-c++ zlib-devel bzip2-devel json-c-devel openssl-devel cmake
      else
        dnf -y install make gcc gcc-c++ zlib-devel bzip2-devel json-c-devel openssl-devel cmake >/dev/null 2>&1
      fi

      # Use the newest cmake available
      # manylinux_2_28 has a pip-installed cmake, or use system cmake
      if command -v /opt/_internal/pipx/venvs/cmake/bin/cmake &>/dev/null; then
        CMAKE_CMD=/opt/_internal/pipx/venvs/cmake/bin/cmake
      else
        CMAKE_CMD=cmake
      fi
      echo "Using CMake: $CMAKE_CMD ($($CMAKE_CMD --version | head -1))"

      echo "==> Running CMake configure for RNP"
      rm -rf /tmp/rnp-build
      mkdir -p /tmp/rnp-build
      cd /tmp/rnp-build

      # Build CMake args
      # manylinux_2_28 has OpenSSL 1.1.1+ in the standard paths
      CMAKE_ARGS=(
        -DCRYPTO_BACKEND="${CRYPTO_BACKEND:-openssl}"
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_TESTING=OFF
        -DENABLE_DOC=OFF
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
      )

      # Configure RNP with OpenSSL backend
      if [ "${VERBOSE:-0}" = "1" ]; then
        $CMAKE_CMD /src/rnp "${CMAKE_ARGS[@]}" 2>&1
      else
        $CMAKE_CMD /src/rnp "${CMAKE_ARGS[@]}" >/dev/null 2>&1
      fi

      # Check generated headers exist
      gen_headers=(
        /tmp/rnp-build/src/lib/config.h
        /tmp/rnp-build/src/lib/version.h
        /tmp/rnp-build/src/lib/rnp/rnp_export.h
      )

      for f in "${gen_headers[@]}"; do
        if [ ! -f "$f" ]; then
          echo "ERROR: expected generated header missing: $f" >&2
          exit 1
        fi
      done

      # Copy outputs
      mkdir -p /out/src/lib
      cp -f /tmp/rnp-build/src/lib/config.h /out/src/lib/config.h
      cp -f /tmp/rnp-build/src/lib/version.h /out/src/lib/version.h

      # Also copy the rnp_export.h if generated (in rnp/ subdirectory)
      if [ -f /tmp/rnp-build/src/lib/rnp/rnp_export.h ]; then
        mkdir -p /out/rnp
        cp -f /tmp/rnp-build/src/lib/rnp/rnp_export.h /out/rnp/rnp_export.h
        echo "    - /out/rnp/rnp_export.h"
      fi

      echo "==> Generated headers:"
      echo "    - /out/src/lib/config.h"
      echo "    - /out/src/lib/version.h"

      # Fix ownership so host user can access files
      chown -R "${HOST_UID}:${HOST_GID}" /out

      rm -rf /tmp/rnp-build
    '

echo
echo "Done. Headers written to: $OUT_DIR"
