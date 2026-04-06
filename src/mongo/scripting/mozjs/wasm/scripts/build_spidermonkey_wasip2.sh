#!/bin/bash
# Builds SpiderMonkey for WASI Preview 2, producing a tarball with:
#   - libjs_static.a  (the JS engine)
#   - headers          (public + internal, .h/.hpp only)
#   - libjsrust.a      (if available)
#   - libmongo_wasip2_rust_shims.a (encoding shims)
#   - obj-extra/       (support objects outside libjs_static.a)
#
# Required environment variables:
#   OUTPUT               - Output tarball path (.tar.gz)
#   SPIDER_MACH_PATH     - Path to SpiderMonkey's mach file
#   RUST_SHIMS_LIB_RS    - Path to support/rust_shims/src/lib.rs
#   CARGO_TEMPLATE_PATH  - Path to support/rust_shims/Cargo.toml.template
#
# Compiler discovery (set one of):
#   WASI_SDK_BIN_FILES   - Space-separated wasi_sdk bin files (auto-discovers clang)
#   WASI_SDK_PATH        - Direct path to WASI SDK root
#
# Example standalone usage:
#   SPIDER_MACH_PATH=/path/to/gecko-dev/mach \
#   RUST_SHIMS_LIB_RS=support/rust_shims/src/lib.rs \
#   CARGO_TEMPLATE_PATH=support/rust_shims/Cargo.toml.template \
#   WASI_SDK_PATH=/opt/wasi-sdk \
#   OUTPUT=spidermonkey-wasip2-release.tar.gz \
#   bash scripts/build_spidermonkey_wasip2.sh

set -euo pipefail
# Enable verbose tracing only when VERBOSE is set (reduces CI log noise).
[[ -n "${VERBOSE:-}" ]] && set -x

EXECROOT="${EXECROOT:-$(pwd -P)}"

# Resolve output to absolute path.
case "$OUTPUT" in
/*) OUT_TAR_ABS="$OUTPUT" ;;
*) OUT_TAR_ABS="$EXECROOT/$OUTPUT" ;;
esac
OUT_DIR_ABS="$(dirname "$OUT_TAR_ABS")"

PKG_DIR="$OUT_DIR_ABS/pkg"
WORK_DIR="$OUT_DIR_ABS/work"
rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR/lib" "$WORK_DIR"

# Keep a real HOME so rustup doesn't trip over Bazel's sandbox home.
REAL_HOME="$(getent passwd "$(id -u)" | cut -d: -f6 || true)"
if [ -z "$REAL_HOME" ]; then
    if [ -n "${HOME:-}" ]; then
        REAL_HOME="$HOME"
    else
        echo "ERROR: Could not determine HOME directory; set HOME or ensure getent is available." >&2
        exit 1
    fi
fi
export HOME="$REAL_HOME"
export PATH="$HOME/.cargo/bin:$PATH"
export MOZ_FETCHES_DIR="$HOME/.mozbuild"

# Install rustup + cargo if not available (e.g. fresh CI hosts).
if ! command -v cargo >/dev/null 2>&1; then
    echo "cargo not found; installing rustup..." >&2
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain stable
    export PATH="$HOME/.cargo/bin:$PATH"
fi

# --- Locate WASI SDK ---
if [ -z "${WASI_SDK_PATH:-}" ]; then
    WASI_CLANG=""
    for f in ${WASI_SDK_BIN_FILES:-}; do
        case "$f" in */clang)
            WASI_CLANG="$f"
            break
            ;;
        esac
    done
    if [ -z "$WASI_CLANG" ]; then
        echo "ERROR: clang not found. Set WASI_SDK_PATH or WASI_SDK_BIN_FILES." >&2
        exit 1
    fi
    WASI_SDK_PATH="$(cd "$(dirname "$WASI_CLANG")/.." && pwd)"
fi

# --- Copy SpiderMonkey source into a writable directory ---
SPIDER_ROOT="$(cd "$(dirname "$SPIDER_MACH_PATH")" && pwd)"
SRC_ROOT="$WORK_DIR/spidermonkey-src"
rm -rf "$SRC_ROOT"
mkdir -p "$SRC_ROOT"
cp -a "$SPIDER_ROOT/." "$SRC_ROOT/"
SRC_ROOT_REAL="$(cd "$SRC_ROOT" && pwd -P)"

MOZCONFIG="$SRC_ROOT_REAL/mozconfig-release"
OBJDIR="$WORK_DIR/obj-release-wasip2"
mkdir -p "$OBJDIR"
OBJDIR_REAL="$(cd "$OBJDIR" && pwd -P)"

cat >"$MOZCONFIG" <<'EOF'
ac_add_options --enable-project=js
ac_add_options --disable-js-shell
ac_add_options --target=wasm32-unknown-wasip2
ac_add_options --without-system-zlib
ac_add_options --without-intl-api
ac_add_options --disable-jit
ac_add_options --disable-shared-js
ac_add_options --disable-shared-memory
ac_add_options --disable-tests
ac_add_options --disable-clang-plugin
ac_add_options --enable-optimize=-Oz
ac_add_options --enable-portable-baseline-interp
mk_add_options AUTOCLOBBER=1
EOF

echo "mk_add_options MOZ_OBJDIR=$OBJDIR_REAL" >>"$MOZCONFIG"
echo "ac_add_options --prefix=$OBJDIR_REAL/dist" >>"$MOZCONFIG"
echo "ac_add_options --disable-stdcxx-compat" >>"$MOZCONFIG"
echo "ac_add_options --disable-debug" >>"$MOZCONFIG"
echo "ac_add_options --disable-debug-symbols" >>"$MOZCONFIG"
echo "ac_add_options --with-sysroot=$WASI_SDK_PATH/share/wasi-sysroot" >>"$MOZCONFIG"

# Use WASI Preview2 compiler wrappers.
export WASI_SDK_PATH="$WASI_SDK_PATH"
export CC="$WASI_SDK_PATH/bin/wasm32-wasip2-clang"
export CXX="$WASI_SDK_PATH/bin/wasm32-wasip2-clang++"
export AR="$WASI_SDK_PATH/bin/llvm-ar"
export RANLIB="$WASI_SDK_PATH/bin/llvm-ranlib"
# HOST_CC is passed in from the Bazel genrule via $(CC). It may be a relative
# path under Bazel's execroot, so resolve it to an absolute path.
if [ -z "${HOST_CC:-}" ]; then
    echo "ERROR: HOST_CC not set; expected from Bazel \$(CC) toolchain variable." >&2
    exit 1
fi
case "$HOST_CC" in
/*) ;; # already absolute
*) HOST_CC="$EXECROOT/$HOST_CC" ;;
esac
case "$HOST_CC" in
*gcc) export HOST_CXX="${HOST_CC/%gcc/g++}" ;;
*clang) export HOST_CXX="${HOST_CC}++" ;;
*cc) export HOST_CXX="${HOST_CC/%cc/c++}" ;;
*) export HOST_CXX="${HOST_CC}++" ;;
esac

# Enable per-function/data sections so the linker's --gc-sections can remove
# unused code.  Also hide internal symbols to aid dead-code elimination.
WASM_SIZE_FLAGS="-ffunction-sections -fdata-sections -fvisibility=hidden"
export CFLAGS="${CFLAGS:-} $WASM_SIZE_FLAGS"
export CXXFLAGS="${CXXFLAGS:-} $WASM_SIZE_FLAGS"

# Let cargo/rustc know what target to build for when SpiderMonkey builds Rust support code.
export RUST_TARGET=wasm32-wasip2

# Configure expects a Rust stdlib for wasm32-wasip2 to be installed.
# This is a local (non-hermetic) build step by design (tags = local/no-sandbox).
if command -v rustup >/dev/null 2>&1; then
    rustup target add wasm32-wasip2 || true
fi

export MOZCONFIG="$MOZCONFIG"

# cbindgen is required by SpiderMonkey's configure step.
if ! command -v cbindgen >/dev/null 2>&1; then
    echo "cbindgen not found; installing via cargo..." >&2
    if ! command -v cargo >/dev/null 2>&1; then
        echo "ERROR: cargo not found; cannot install cbindgen" >&2
        exit 1
    fi
    cargo install cbindgen
fi

cd "$SRC_ROOT_REAL"
python3 "./mach" -v --no-interactive build

# Build Rust support library if present/required (best-effort).
python3 "./mach" -v --no-interactive build js/src/rust || true

LIBJS_A="$OBJDIR_REAL/js/src/build/libjs_static.a"
test -s "$LIBJS_A"
cp "$LIBJS_A" "$PKG_DIR/lib/libjs_static.a"

# Headers for embedders (needed to compile our wrapper).
if [ -d "$OBJDIR_REAL/dist/include" ]; then
    cp -Lr "$OBJDIR_REAL/dist/include" "$PKG_DIR/include"
    # Generated defines header is used by some includes.
    if [ -f "$OBJDIR_REAL/js/src/js-confdefs.h" ]; then
        cp "$OBJDIR_REAL/js/src/js-confdefs.h" "$PKG_DIR/include/js-confdefs.h"
    fi
fi

# Internal SpiderMonkey headers (js/src) for gc/GCContext.h, vm/Runtime.h etc.
# Copy source headers first, then overlay generated headers from objdir.
if [ -d "$SRC_ROOT_REAL/js/src" ]; then
    echo "Copying js/src internal headers (*.h, *.hpp only) from source..." >&2
    mkdir -p "$PKG_DIR/include/src"
    # Copy only header files -- not .cpp, tests, or build configs which bloat
    # the tarball by hundreds of megabytes.
    (cd "$SRC_ROOT_REAL/js/src" && find . \( -name '*.h' -o -name '*.hpp' \) -type f) | while IFS= read -r hdr; do
        mkdir -p "$PKG_DIR/include/src/$(dirname "$hdr")"
        cp "$SRC_ROOT_REAL/js/src/$hdr" "$PKG_DIR/include/src/$hdr"
    done
fi
if [ -d "$SRC_ROOT_REAL/mfbt" ]; then
    echo "Copying mfbt headers (*.h, *.hpp only) from source..." >&2
    mkdir -p "$PKG_DIR/include/mfbt"
    (cd "$SRC_ROOT_REAL/mfbt" && find . \( -name '*.h' -o -name '*.hpp' \) -type f) | while IFS= read -r hdr; do
        mkdir -p "$PKG_DIR/include/mfbt/$(dirname "$hdr")"
        cp "$SRC_ROOT_REAL/mfbt/$hdr" "$PKG_DIR/include/mfbt/$hdr"
    done
fi
# Overlay generated .h files from objdir (e.g., wasm/WasmBuiltinModuleGenerated.h)
if [ -d "$OBJDIR_REAL/js/src" ]; then
    echo "Overlaying generated headers from objdir..." >&2
    find "$OBJDIR_REAL/js/src" -name "*.h" -type f 2>/dev/null | while read f; do
        rel="${f#$OBJDIR_REAL/js/src/}"
        mkdir -p "$PKG_DIR/include/src/$(dirname "$rel")"
        cp "$f" "$PKG_DIR/include/src/$rel" 2>/dev/null || true
    done
fi

# If a Rust static lib exists, include it (may satisfy encoding_* symbols).
# Name can vary by toolchain/version, so accept libjsrust*.a.
RUST_LIB="$(find "$OBJDIR_REAL" -type f -name 'libjsrust*.a' | head -n1 || true)"
if [ -n "$RUST_LIB" ] && [ -f "$RUST_LIB" ]; then
    cp "$RUST_LIB" "$PKG_DIR/lib/libjsrust.a"
fi

# Build a single Rust staticlib that bundles the encoding shims + Rust
# runtime, so the final wasm can be fully linked without `env::...`
# imports.
CARGO="$(command -v cargo || true)"
if [ -z "$CARGO" ] && [ -x "$HOME/.cargo/bin/cargo" ]; then
    CARGO="$HOME/.cargo/bin/cargo"
fi
if [ -z "$CARGO" ]; then
    echo "ERROR: cargo not found; cannot build Rust encoding staticlib" >&2
    exit 1
fi

RUST_SHIMS_DIR="$WORK_DIR/rust-shims"
rm -rf "$RUST_SHIMS_DIR"
mkdir -p "$RUST_SHIMS_DIR/src"

# Copy lib.rs from support files.
# Path may be relative to EXECROOT or absolute.
case "$RUST_SHIMS_LIB_RS" in
/*) cp "$RUST_SHIMS_LIB_RS" "$RUST_SHIMS_DIR/src/lib.rs" ;;
*) cp "$EXECROOT/$RUST_SHIMS_LIB_RS" "$RUST_SHIMS_DIR/src/lib.rs" ;;
esac

# Generate Cargo.toml from template with SpiderMonkey root path.
case "$CARGO_TEMPLATE_PATH" in
/*) sed "s|{{SPIDERMONKEY_ROOT}}|$SRC_ROOT_REAL|g" "$CARGO_TEMPLATE_PATH" >"$RUST_SHIMS_DIR/Cargo.toml" ;;
*) sed "s|{{SPIDERMONKEY_ROOT}}|$SRC_ROOT_REAL|g" "$EXECROOT/$CARGO_TEMPLATE_PATH" >"$RUST_SHIMS_DIR/Cargo.toml" ;;
esac

export CARGO_TARGET_DIR="$OBJDIR_REAL/cargo-target"
(cd "$RUST_SHIMS_DIR" && "$CARGO" build --release --target wasm32-wasip2)
SHIMS_A="$CARGO_TARGET_DIR/wasm32-wasip2/release/libmongo_wasip2_rust_shims.a"
if [ ! -f "$SHIMS_A" ]; then
    echo "ERROR: rust shims archive not found: $SHIMS_A" >&2
    exit 1
fi
cp "$SHIMS_A" "$PKG_DIR/lib/libmongo_wasip2_rust_shims.a"

# Include additional build outputs (outside libjs_static.a) needed to link.
# The downstream linker uses libjs_static.a directly for SpiderMonkey objects,
# so we do NOT extract individual .o files from the archive (which would
# roughly double the tarball size).  Only the "extra" objects that live outside
# the archive are needed here.
#
# Paths are relative to MOZ_OBJDIR (OBJDIR_REAL).
EXTRA_ROOT="$PKG_DIR/obj-extra"
for rel in \
    memory/build/Unified_cpp_memory_build0.o \
    memory/mozalloc/Unified_cpp_memory_mozalloc0.o \
    mozglue/misc/AutoProfilerLabel.o \
    mozglue/misc/ConditionVariable_noop.o \
    mozglue/misc/MmapFaultHandler.o \
    mozglue/misc/Mutex_noop.o \
    mozglue/misc/Now.o \
    mozglue/misc/Printf.o \
    mozglue/misc/StackWalk.o \
    mozglue/misc/TimeStamp.o \
    mozglue/misc/TimeStamp_posix.o \
    mozglue/misc/Uptime.o \
    mozglue/misc/Decimal.o \
    mozglue/misc/SIMD.o \
    mfbt/lz4.o \
    mfbt/lz4frame.o \
    mfbt/lz4hc.o \
    mfbt/xxhash.o \
    mfbt/Unified_cpp_mfbt0.o \
    mfbt/Unified_cpp_mfbt1.o; do
    if [ -f "$OBJDIR_REAL/$rel" ]; then
        mkdir -p "$EXTRA_ROOT/$(dirname "$rel")"
        cp "$OBJDIR_REAL/$rel" "$EXTRA_ROOT/$rel"
    else
        echo "WARN: extra object missing (skipping): $OBJDIR_REAL/$rel" >&2
    fi
done

# Strip debug info from archives and objects to further reduce tarball size.
# Even with --disable-debug-symbols the toolchain may leave some sections.
STRIP="${WASI_SDK_PATH}/bin/llvm-strip"
if [ -x "$STRIP" ]; then
    echo "Stripping debug info from libraries and objects..." >&2
    for lib in "$PKG_DIR"/lib/*.a; do
        [ -f "$lib" ] && "$STRIP" --strip-debug "$lib" 2>/dev/null || true
    done
    find "$PKG_DIR/obj-extra" -name '*.o' -type f 2>/dev/null | while IFS= read -r obj; do
        "$STRIP" --strip-debug "$obj" 2>/dev/null || true
    done
else
    echo "WARN: llvm-strip not found at $STRIP; skipping strip step" >&2
fi

mkdir -p "$(dirname "$OUT_TAR_ABS")"
tar -C "$PKG_DIR" -czf "$OUT_TAR_ABS" .
test -s "$OUT_TAR_ABS"
