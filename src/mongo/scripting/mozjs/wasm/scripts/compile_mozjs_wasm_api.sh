#!/bin/bash
# Compiles and links the MozJS WASM API module (mozjs_wasm_api.wasm).
#
# This script unpacks a SpiderMonkey tarball, compiles MozJS wrapper sources
# against it, collects MongoDB base libraries, and links everything into a
# single WASI Preview 2 WASM component.
#
# Required environment variables:
#   OUTPUT                    - Output .wasm file path
#   SM_TARBALL                - Path to spidermonkey-wasip2-release.tar.gz
#   RUST_SHIMS_PATH           - Path to extracted rust_shims.a
#   WIT_COMPONENT_TYPE_OBJ    - Path to WIT component type object file
#
# Source files (space-separated paths):
#   WASM_SOURCES              - MozJS WASM wrapper sources
#   COMMON_WASI_SOURCES       - Common WASI source files
#   EXCEPTION_STUBS_SRC       - engine/exception_stubs.cpp
#   MOZJS_API_SRC             - engine/api.cpp
#   WIT_API_C                 - WIT-generated api.c glue
#
# Header discovery:
#   ERROR_CODES_HEADER_FILES  - Space-separated error_codes header file paths
#   CONFIG_HEADER_FILES       - Space-separated mongo_config header file paths
#
# Linker inputs:
#   LINKSET_FILES             - Space-separated linkset output files
#
# Compiler discovery (set one of):
#   CXX / CC                  - Direct paths to wasm32-wasip2-clang++ / clang
#   WASI_SDK_BIN_FILES        - Space-separated wasi_sdk bin files (auto-discovers)
#
# Compile helper (optional, auto-discovered from script directory):
#   COMPILE_HELPER            - Path to compile_wasi_source.sh
#
# Example standalone usage:
#   CXX=/opt/wasi-sdk/bin/wasm32-wasip2-clang++ \
#   CC=/opt/wasi-sdk/bin/wasm32-wasip2-clang \
#   SM_TARBALL=spidermonkey-wasip2-release.tar.gz \
#   RUST_SHIMS_PATH=rust_shims.a \
#   WASM_SOURCES="engine.cpp error.cpp helpers.cpp" \
#   WIT_COMPONENT_TYPE_OBJ=api_component_type.o \
#   OUTPUT=mozjs_wasm_api.wasm \
#   bash scripts/compile_mozjs_wasm_api.sh

set -euo pipefail

EXECROOT="${EXECROOT:-$(pwd -P)}"
START_TIME=$(date +%s)

# ============================================================
# STEP 1: Find compilers and setup
# ============================================================
echo "=== STEP 1: Finding WASI compilers ===" >&2
if [ -z "${CXX:-}" ] || [ -z "${CC:-}" ]; then
    for f in ${WASI_SDK_BIN_FILES:-}; do
        case "$f" in
        */wasm32-wasip2-clang++) CXX="$f" ;;
        */wasm32-wasip2-clang) CC="$f" ;;
        esac
    done
fi
if [ -z "${CXX:-}" ]; then
    echo "ERROR: wasm32-wasip2-clang++ not found. Set CXX or WASI_SDK_BIN_FILES." >&2
    exit 1
fi
if [ -z "${CC:-}" ]; then
    echo "ERROR: wasm32-wasip2-clang not found. Set CC or WASI_SDK_BIN_FILES." >&2
    exit 1
fi

WASI_SDK_ROOT="$(cd "$(dirname "$CXX")/.." && pwd -P)"
SYSROOT="${SYSROOT:-$WASI_SDK_ROOT/share/wasi-sysroot}"
echo "Using CXX: $CXX" >&2
echo "Using  CC: $CC" >&2

# ============================================================
# STEP 2: Unpack SpiderMonkey
# ============================================================
echo "=== STEP 2: Unpacking SpiderMonkey ===" >&2
OUT_DIR="$(dirname "$OUTPUT")"
STAGE="$OUT_DIR/sm_stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"
tar -xzf "$SM_TARBALL" -C "$STAGE"

test -f "$STAGE/lib/libjs_static.a" || (
    echo "ERROR: libjs_static.a not found" >&2
    exit 1
)
test -d "$STAGE/include" || (
    echo "ERROR: include directory not found" >&2
    exit 1
)

cp "$RUST_SHIMS_PATH" "$STAGE/lib/libmongo_wasip2_rust_shims.a"
echo "SpiderMonkey unpacked to: $STAGE" >&2

# ============================================================
# STEP 3: Setup compilation environment
# ============================================================
echo "=== STEP 3: Setting up compilation environment ===" >&2
OBJ_DIR="$STAGE/objs"
mkdir -p "$OBJ_DIR"

ERROR_CODES_H_PARENT=""
CONFIG_H_PARENT=""
for f in ${ERROR_CODES_HEADER_FILES:-}; do
    ERROR_CODES_H_PARENT="$(dirname "$(dirname "$f")")"
    break
done
for f in ${CONFIG_HEADER_FILES:-}; do
    CONFIG_H_PARENT="$(dirname "$(dirname "$f")")"
    break
done

BOOST_INCLUDES="-I$EXECROOT/src/third_party/boost"
[ -d "$EXECROOT/src/third_party/boost/libs/numeric/conversion/include" ] &&
    BOOST_INCLUDES="$BOOST_INCLUDES -I$EXECROOT/src/third_party/boost/libs/numeric/conversion/include"
for lib_include in "$EXECROOT/src/third_party/boost/libs"/*/include; do
    [ -d "$lib_include" ] && BOOST_INCLUDES="$BOOST_INCLUDES -I$lib_include"
done

# Auto-detect Bazel bin/src path for generated headers (e.g., error_codes.h).
# This must work on any host architecture (aarch64, x86_64, etc.).
BAZEL_BIN_SRC=""
for d in "$EXECROOT"/bazel-out/*/bin/src; do
    if [ -d "$d" ]; then
        BAZEL_BIN_SRC="$d"
        break
    fi
done

# ============================================================
# STEP 4: Compile MozJS library sources (including WIT glue)
# ============================================================
echo "=== STEP 4: Compiling MozJS library sources ===" >&2
MOZJS_SRCS="${WASM_SOURCES:-}"
MOZJS_SRCS="$MOZJS_SRCS ${COMMON_WASI_SOURCES:-}"
MOZJS_SRCS="$MOZJS_SRCS ${EXCEPTION_STUBS_SRC:-}"
MOZJS_SRCS="$MOZJS_SRCS ${MOZJS_API_SRC:-}"

# Add wit-bindgen generated glue (C) to compilation inputs.
MOZJS_SRCS="$MOZJS_SRCS ${WIT_API_C:-}"

SRC_LIST="$STAGE/src_list.txt"
>"$SRC_LIST"
# Note: intentionally unquoted to split space-separated paths (Bazel paths never contain spaces).
for src in $MOZJS_SRCS; do
    [ -n "$src" ] && echo "$src" >>"$SRC_LIST"
done

# Locate the compile helper script.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPILE_HELPER="${COMPILE_HELPER:-$SCRIPT_DIR/compile_wasi_source.sh}"

NPROC=$(nproc 2>/dev/null || echo 8)
echo "Compiling $(wc -l <"$SRC_LIST") files with $NPROC parallel jobs" >&2

OBJ_LIST="$STAGE/obj_list.txt"
if ! cat "$SRC_LIST" | xargs -P "$NPROC" -I {} bash "$COMPILE_HELPER" {} \
    "$CXX" "$CC" "$SYSROOT" "$STAGE" "$OBJ_DIR" "$EXECROOT" \
    "$ERROR_CODES_H_PARENT" "$CONFIG_H_PARENT" "$BOOST_INCLUDES" \
    "$BAZEL_BIN_SRC" >"$OBJ_LIST" 2>&1; then
    echo "ERROR: Parallel compilation failed" >&2
    cat "$OBJ_LIST" >&2
    exit 1
fi

OBJ_FILES=""
for obj in "$OBJ_DIR"/*.o; do
    [ -f "$obj" ] && OBJ_FILES="$OBJ_FILES $obj"
done
COMPILE_COUNT=$(ls -1 "$OBJ_DIR"/*.o 2>/dev/null | wc -l)
echo "Compiled $COMPILE_COUNT files successfully" >&2

# ============================================================
# STEP 5: Collect MongoDB base libraries
# ============================================================
echo "=== STEP 5: Collecting MongoDB base libraries ===" >&2

MONGO_BASE_LIBS=""
MONGO_BASE_LIBS_RSP=""
for f in ${LINKSET_FILES:-}; do
    case "$f" in
    *.libs.rsp) MONGO_BASE_LIBS_RSP="$f" ;;
    esac
done

# Use only the .a archives from libs.rsp (they contain all objects).
# We skip objects.rsp to avoid duplicate symbols since the archives
# contain the same objects that would appear there.
if [ -n "$MONGO_BASE_LIBS_RSP" ] && [ -f "$MONGO_BASE_LIBS_RSP" ]; then
    LIB_COUNT=0
    while IFS= read -r lib_path || [ -n "$lib_path" ]; do
        [ -z "$lib_path" ] && continue
        case "$lib_path" in "#"*) continue ;; esac
        if [ ! "$lib_path" = "${lib_path#/}" ]; then
            LIB_ABS="$lib_path"
        else
            LIB_ABS="$EXECROOT/$lib_path"
        fi
        if [ -f "$LIB_ABS" ]; then
            MONGO_BASE_LIBS="$MONGO_BASE_LIBS $LIB_ABS"
            LIB_COUNT=$((LIB_COUNT + 1))
        fi
    done <"$MONGO_BASE_LIBS_RSP"
    echo "Found $LIB_COUNT libraries from libs.rsp" >&2
fi

# ============================================================
# STEP 6: Final linking (include wit component type object)
# ============================================================
echo "=== STEP 6: Linking final WASM binary ===" >&2
EXTRA_OBJS="$(find "$STAGE/obj-extra" -type f -name '*.o' 2>/dev/null | tr '\n' ' ' || true)"
JSRUST_LIB=""
[ -f "$STAGE/lib/libjsrust.a" ] && JSRUST_LIB="$STAGE/lib/libjsrust.a"

echo "Linking with:" >&2
echo "  MozJS objects: $(echo $OBJ_FILES | wc -w)" >&2
echo "  Mongo base libs: $(echo $MONGO_BASE_LIBS | wc -w)" >&2

if ! "$CXX" --sysroot="$SYSROOT" -std=c++20 -Oz \
    -flto -fexceptions \
    $OBJ_FILES \
    "$WIT_COMPONENT_TYPE_OBJ" \
    "$STAGE/lib/libjs_static.a" \
    "$STAGE/lib/libmongo_wasip2_rust_shims.a" \
    $JSRUST_LIB \
    $EXTRA_OBJS \
    $MONGO_BASE_LIBS \
    -L"$SYSROOT/lib/wasm32-wasip2" \
    -lc++ -lc++abi \
    -lwasi-emulated-getpid \
    -lwasi-emulated-signal \
    -lwasi-emulated-mman \
    -lwasi-emulated-process-clocks \
    -mexec-model=reactor \
    -Wl,--export=cabi_realloc \
    -Wl,--gc-sections \
    -Wl,--strip-all \
    -Wl,--no-entry \
    -o "$OUTPUT" 2>&1; then
    echo "ERROR: Linking failed" >&2
    exit 1
fi

test -s "$OUTPUT"
ls -lh "$OUTPUT"

# Post-link size optimization with wasm-opt.
# wasm-opt can typically shrink a WASM binary by another 10-20% through
# WASM-specific optimisations (dead code removal, constant folding, etc.).
WASM_OPT="$(command -v wasm-opt 2>/dev/null || true)"
if [ -z "$WASM_OPT" ] && [ -x "$WASI_SDK_ROOT/bin/wasm-opt" ]; then
    WASM_OPT="$WASI_SDK_ROOT/bin/wasm-opt"
fi
if [ -n "$WASM_OPT" ]; then
    echo "Running wasm-opt -Oz on $OUTPUT ..." >&2
    "$WASM_OPT" -Oz --enable-bulk-memory --enable-sign-ext \
        -o "$OUTPUT.opt" "$OUTPUT" 2>&1 && mv "$OUTPUT.opt" "$OUTPUT"
    ls -lh "$OUTPUT"
else
    echo "NOTE: wasm-opt not found; skipping post-link optimisation" >&2
fi

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))
echo "=== SUCCESS: Built mozjs_wasm_api.wasm in ${ELAPSED}s ===" >&2
