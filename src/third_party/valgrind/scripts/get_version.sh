#!/usr/bin/env bash
# Extracts the version from the Valgrind header file.
set -euo pipefail
HEADER="src/third_party/valgrind/include/valgrind/valgrind.h"
MAJOR=$(grep '^#define __VALGRIND_MAJOR__' "$HEADER" | awk '{print $3}')
MINOR=$(grep '^#define __VALGRIND_MINOR__' "$HEADER" | awk '{print $3}')
echo "${MAJOR}.${MINOR}"
