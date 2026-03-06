#!/bin/bash
# Compiles a single C/C++ source file for WASI.
# Used as a helper by compile_mozjs_wasm_api.sh for parallel compilation via xargs.
#
# Positional arguments:
#   $1  - Source file path
#   $2  - C++ compiler (wasm32-wasip2-clang++)
#   $3  - C compiler (wasm32-wasip2-clang)
#   $4  - Sysroot path
#   $5  - Stage directory (contains unpacked SpiderMonkey headers)
#   $6  - Object output directory
#   $7  - Execution root (repo root)
#   $8  - Error codes header parent directory
#   $9  - Config header parent directory
#   $10 - Boost include flags
#   $11 - Bazel bin/src path (e.g., bazel-out/k8-fastbuild/bin/src)
#
# Example standalone usage:
#   bash scripts/compile_wasi_source.sh myfile.cpp \
#     /opt/wasi-sdk/bin/wasm32-wasip2-clang++ \
#     /opt/wasi-sdk/bin/wasm32-wasip2-clang \
#     /opt/wasi-sdk/share/wasi-sysroot \
#     /tmp/sm_stage /tmp/objs /path/to/repo \
#     /path/to/error_codes_parent /path/to/config_parent \
#     "-I/path/to/boost" \
#     /path/to/bazel-out/k8-fastbuild/bin/src

set -euo pipefail

src="$1"
CXX="$2"
CC="$3"
SYSROOT="$4"
STAGE="$5"
OBJ_DIR="$6"
EXECROOT="$7"
ERROR_CODES_H_PARENT="$8"
CONFIG_H_PARENT="$9"
BOOST_INCLUDES="${10}"
BAZEL_BIN_SRC="${11:-}"

# Auto-detect bazel-out bin/src path if not provided.
# Searches for any bazel-out/*/bin/src directory under EXECROOT.
if [ -z "$BAZEL_BIN_SRC" ]; then
    for d in "$EXECROOT"/bazel-out/*/bin/src; do
        if [ -d "$d" ]; then
            BAZEL_BIN_SRC="$d"
            break
        fi
    done
fi

# Derive a unique object name by prefixing with the parent directory.
# This avoids collisions when files from different directories share the
# same basename (e.g. common/error.cpp vs engine/error.cpp, or
# generated/api.c vs engine/api.cpp).
parent="$(basename "$(dirname "$src")")"
base="$(basename "$src")"
OBJ_NAME="${parent}_${base%.*}.o"
OBJ_PATH="$OBJ_DIR/$OBJ_NAME"

ext="${src##*.}"
COMP="$CXX"
STD="-std=c++20"
if [ "$ext" = "c" ]; then
    COMP="$CC"
    STD="-std=c17"
fi

# Build include flags for Bazel-generated headers (e.g., error_codes.h, config.h).
BAZEL_BIN_INCLUDE=""
if [ -n "$BAZEL_BIN_SRC" ] && [ -d "$BAZEL_BIN_SRC" ]; then
    BAZEL_BIN_INCLUDE="-I$BAZEL_BIN_SRC"
fi

"$COMP" --sysroot="$SYSROOT" $STD -Oz \
    -flto -fexceptions -ffunction-sections -fdata-sections \
    -fvisibility=hidden \
    -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_MMAN \
    -D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_GETPID \
    -DMOZ_HAS_MOZGLUE \
    -I"$STAGE/include" -I"$STAGE/include/js" -I"$STAGE/include/mozilla" \
    -I"$STAGE/include/src" -I"$STAGE/include/mfbt" \
    -include "$STAGE/include/js-confdefs.h" \
    -I"$EXECROOT/src" $BAZEL_BIN_INCLUDE \
    -I"$EXECROOT/src/mongo/scripting/mozjs/wasm" \
    -I"$EXECROOT/src/mongo/scripting/mozjs/common" \
    -I"$EXECROOT/src/mongo/scripting/mozjs/common/types" \
    -I"$EXECROOT/src/third_party/abseil-cpp/dist" \
    -I"$EXECROOT/src/mongo/scripting/mozjs/wasm/wit_gen/generated" \
    -DMONGO_MOZJS_WASI_BUILD \
    $BOOST_INCLUDES \
    -I"$ERROR_CODES_H_PARENT" -I"$CONFIG_H_PARENT" \
    -c "$src" -o "$OBJ_PATH" 2>&1

echo "$OBJ_PATH"
