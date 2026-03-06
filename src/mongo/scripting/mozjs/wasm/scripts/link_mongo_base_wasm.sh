#!/bin/bash
# Links mongo_base.wasm from WASI linkset response files.
#
# Required environment variables:
#   OUTPUT              - Output .wasm file path
#
# Compiler discovery (set one of):
#   CXX                 - Direct path to wasm32-wasip2-clang++
#   WASI_SDK_BIN_FILES  - Space-separated wasi_sdk bin files (auto-discovers compiler)
#
# RSP files (set one of):
#   OBJS_RSP / LIBS_RSP / FLAGS_RSP  - Direct paths to response files
#   LINKSET_FILES                     - Space-separated linkset outputs (auto-discovers RSPs)
#
# Example standalone usage:
#   CXX=/path/to/wasm32-wasip2-clang++ \
#   OBJS_RSP=objs.rsp LIBS_RSP=libs.rsp FLAGS_RSP=flags.rsp \
#   OUTPUT=mongo_base.wasm \
#   bash scripts/link_mongo_base_wasm.sh

set -euo pipefail

# --- Discover compiler ---
# Prefer wasip2 (WASI Preview 2) compiler to match the rest of the build.
if [ -z "${CXX:-}" ]; then
    for f in ${WASI_SDK_BIN_FILES:-}; do
        case "$f" in */wasm32-wasip2-clang++)
            CXX="$f"
            break
            ;;
        esac
    done
fi
# Fallback to WASI Preview 1 compiler name if wasip2 variant not found.
if [ -z "${CXX:-}" ]; then
    for f in ${WASI_SDK_BIN_FILES:-}; do
        case "$f" in */wasm32-wasi-clang++)
            CXX="$f"
            break
            ;;
        esac
    done
fi
if [ -z "${CXX:-}" ]; then
    echo "ERROR: wasm32-wasip2-clang++ (or wasm32-wasi-clang++) not found. Set CXX or WASI_SDK_BIN_FILES." >&2
    exit 1
fi

# --- Discover RSP files ---
if [ -z "${OBJS_RSP:-}" ] || [ -z "${LIBS_RSP:-}" ] || [ -z "${FLAGS_RSP:-}" ]; then
    for f in ${LINKSET_FILES:-}; do
        case "$f" in
        *.objects.rsp) OBJS_RSP="$f" ;;
        *.libs.rsp) LIBS_RSP="$f" ;;
        *.flags.rsp) FLAGS_RSP="$f" ;;
        esac
    done
fi
if [ -z "${OBJS_RSP:-}" ] || [ -z "${LIBS_RSP:-}" ] || [ -z "${FLAGS_RSP:-}" ]; then
    echo "ERROR: Missing RSP files (.objects/.libs/.flags)." >&2
    echo "Set OBJS_RSP/LIBS_RSP/FLAGS_RSP or LINKSET_FILES." >&2
    exit 1
fi

# Select the correct --target based on the compiler name.
# wasip2 compiler requires wasip2 target; wasi compiler requires wasi target.
WASM_TARGET="wasm32-wasip2"

echo "Linking mongo_base.wasm" >&2
echo "  CXX:       $CXX" >&2
echo "  TARGET:    $WASM_TARGET" >&2
echo "  OBJS_RSP:  $OBJS_RSP" >&2
echo "  LIBS_RSP:  $LIBS_RSP" >&2
echo "  FLAGS_RSP: $FLAGS_RSP" >&2

"$CXX" --target="$WASM_TARGET" -std=c++20 -Oz -fexceptions \
    @"$OBJS_RSP" @"$LIBS_RSP" @"$FLAGS_RSP" \
    -Wl,--gc-sections \
    -Wl,--strip-all \
    -o "$OUTPUT"

echo "Output: $OUTPUT" >&2
