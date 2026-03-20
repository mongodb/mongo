#!/bin/bash
# Build script for json-c generated headers (config.h, json_config.h)
#
# json-c uses CMake and generates headers that are NOT platform-specific
# within 64-bit Linux (all use LP64 data model).
#
# Usage:
#   ./build_json_c_headers.sh
#   VERBOSE=1 ./build_json_c_headers.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JSON_C_SRC="$SCRIPT_DIR/../dist"
OUT_DIR="$SCRIPT_DIR/../dist/build"

# Configuration
VERBOSE="${VERBOSE:-0}"
PULL="${PULL:-1}"

# Use native architecture - headers are not platform-specific on 64-bit Linux
# Using manylinux_2_28 (AlmaLinux 8 / glibc 2.28) to match modern Linux systems
# (manylinux2014 has old glibc with xlocale.h which was removed in glibc 2.26)
ARCH=$(uname -m)
case "$ARCH" in
  x86_64)  DOCKER_IMAGE="quay.io/pypa/manylinux_2_28_x86_64" ;;
  aarch64) DOCKER_IMAGE="quay.io/pypa/manylinux_2_28_aarch64" ;;
  *)       echo "Unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

# Check source exists
if [ ! -d "$JSON_C_SRC" ]; then
  echo "ERROR: json-c source not found at $JSON_C_SRC" >&2
  echo "Run import.sh first." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

echo "==> json-c header generator"
echo "    Source: $JSON_C_SRC"
echo "    Output: $OUT_DIR"
echo "    VERBOSE: $VERBOSE"

if [ "$PULL" = "1" ]; then
  echo "==> Pulling image: $DOCKER_IMAGE"
  docker pull "$DOCKER_IMAGE"
fi

docker run --rm -t \
    -e VERBOSE="$VERBOSE" \
    -e HOST_UID="$(id -u)" \
    -e HOST_GID="$(id -g)" \
    -v "$JSON_C_SRC":/src/json-c:ro \
    -v "$OUT_DIR":/out \
    "$DOCKER_IMAGE" \
    bash -lc '
      set -euo pipefail

      # Install build tools
      # manylinux_2_28 (AlmaLinux 8) uses dnf
      if [ "${VERBOSE:-0}" = "1" ]; then
        dnf -y install make gcc gcc-c++ cmake
      else
        dnf -y install make gcc gcc-c++ cmake >/dev/null 2>&1
      fi

      # Use pip-installed cmake if available (newer), otherwise system cmake
      if command -v /opt/_internal/pipx/venvs/cmake/bin/cmake &>/dev/null; then
        CMAKE_CMD=/opt/_internal/pipx/venvs/cmake/bin/cmake
      else
        CMAKE_CMD=cmake
      fi

      echo "==> Running CMake configure for json-c"
      rm -rf /tmp/json-c-build
      mkdir -p /tmp/json-c-build
      cd /tmp/json-c-build

      if [ "${VERBOSE:-0}" = "1" ]; then
        $CMAKE_CMD /src/json-c \
          -DBUILD_SHARED_LIBS=OFF \
          -DBUILD_STATIC_LIBS=ON \
          -DBUILD_TESTING=OFF \
          -DBUILD_APPS=OFF \
          -DDISABLE_EXTRA_LIBS=ON \
          -DDISABLE_WERROR=ON \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
          2>&1
      else
        $CMAKE_CMD /src/json-c \
          -DBUILD_SHARED_LIBS=OFF \
          -DBUILD_STATIC_LIBS=ON \
          -DBUILD_TESTING=OFF \
          -DBUILD_APPS=OFF \
          -DDISABLE_EXTRA_LIBS=ON \
          -DDISABLE_WERROR=ON \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
          >/dev/null 2>&1
      fi

      # Check generated headers exist
      gen_headers=(
        /tmp/json-c-build/config.h
        /tmp/json-c-build/json_config.h
        /tmp/json-c-build/json.h
      )

      for f in "${gen_headers[@]}"; do
        if [ ! -f "$f" ]; then
          echo "ERROR: expected generated header missing: $f" >&2
          exit 1
        fi
      done

      # Copy outputs
      cp -f /tmp/json-c-build/config.h /out/config.h
      cp -f /tmp/json-c-build/json_config.h /out/json_config.h
      cp -f /tmp/json-c-build/json.h /out/json.h

      echo "==> Generated headers:"
      echo "    - /out/config.h"
      echo "    - /out/json_config.h"
      echo "    - /out/json.h"

      # Fix ownership so host user can access files
      chown -R "${HOST_UID}:${HOST_GID}" /out

      rm -rf /tmp/json-c-build
    '

echo
echo "Done. Headers written to: $OUT_DIR"
