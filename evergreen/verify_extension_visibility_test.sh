#!/usr/bin/env bash
#
# Purpose: Enforce strict “safe-to-dlopen” rules for a MongoDB extension .so by inspecting:
#     1) Exported symbols (nm)
#        - The extension must export ONLY the C API entry point get_mongodb_extension
#     2) Dynamic dependencies (ldd)
#        - Enumerate the extension’s runtime shared-library dependencies
#        - Compare against the server’s runtime dependencies to detect major-version conflicts (per MongoDB version)
#        - For any library dynamically linked by both the server and the extension, fail if their required major sonames differ
#        - libstdc++ is explicitly allowed but must be runtime-compatible with the server (checked via soname/major version)
#     3) Symbol override hazards (nm)
#        - Detect common defined dynamic symbols between the server and the extension and reject any unexpected overlaps
#        - The extension must NOT define allocator symbols (malloc, free, calloc, realloc, etc.)
#
# This script is intentionally strict: any unexpected symbol exports, dependency mismatches, or override risks cause a hard failure.
#
# Usage
#   ./verify_extension_visibility_test.sh /path/to/mongod /path/to/extension.so
#

set -euo pipefail

die() {
    echo "ERROR: $*" >&2
    exit 1
}
info() { echo "[verify_extension] $*" >&2; }

if [[ "$(uname)" != "Linux" ]]; then
    info "Skipping: Linux-only"
    exit 0
fi

SERVER_BIN="${1:-}"
EXT_SO="${2:-}"
[[ -n "$SERVER_BIN" && -x "$SERVER_BIN" ]] || die "Usage: $0 /path/to/mongod /path/to/extension.so"
[[ -n "$EXT_SO" && -f "$EXT_SO" ]] || die "Extension .so not found: $EXT_SO"

# ------------------------------------------------------------------------------
# 1) Exported symbol surface check (nm)
#    Rule: extension exports ONLY get_mongodb_extension.
#
# We inspect the dynamic symbol table (-D) and consider only symbols that are defined by the extension (--defined-only).
# Undefined symbols ('U' / 'w') are references and do not count as exports.
# ------------------------------------------------------------------------------
info "1) Checking extension exports only get_mongodb_extension"

ext_defined_exports="$(
    nm -D --defined-only "$EXT_SO" |
        awk '{print $NF}' |
        sed 's/@@.*$//' |
        sort -u
)"

# Strict expected set.
expected_exports=$'get_mongodb_extension'

if [[ "$ext_defined_exports" != "$expected_exports" ]]; then
    echo "Expected exported defined dynsym set:"
    echo "$expected_exports"
    echo
    echo "Actual exported defined dynsym set:"
    echo "$ext_defined_exports"
    die "Extension exports unexpected symbols. Enforce -fvisibility=hidden and export only get_mongodb_extension."
fi

info "Export surface check passed."

# ------------------------------------------------------------------------------
# 2) Dynamic dependency checks (ldd)
#    Rules:
#      - Extension should avoid unexpected dynamic deps (prefer static linking)
#      - Extension/server must not require different major versions of the same library
#
# We use ldd for “what will be loaded at runtime”.
# For conflict checks, we normalize to major SONAMEs (e.g., libfoo.so.3.1 -> libfoo.so.3)
# and compare majors between server and extension for libraries present in both.
# ------------------------------------------------------------------------------
info "2) Checking dynamic dependencies and obvious version conflicts"

# Extract the soname basename from ldd lines, e.g.:
#   libssl.so.3 => /.../libssl.so.3
#   /lib64/ld-linux-x86-64.so.2
ldd_libs_basename() {
    local bin="$1"
    ldd "$bin" |
        awk '
        $1 ~ /^\// {print $1}
        $2 == "=>" && $3 ~ /^\// {print $3}
      ' |
        xargs -r -n1 basename |
        sort -u
}

# Extract DT_NEEDED entries (direct dependencies) from readelf output
dt_needed_libs() {
    local bin="$1"
    readelf -d "$bin" |
        awk '/\(NEEDED\)/ {gsub(/\[|\]/, "", $5); print $5}' |
        sort -u
}

srv_ldd="$(ldd_libs_basename "$SERVER_BIN")"
ext_ldd="$(ldd_libs_basename "$EXT_SO")"

# Base regex for allowed dependencies (common to both direct and transitive)
# NOTE: This is still a shared object (dlopen), so libc + loader will be dynamic.
# Policy exceptions: OpenSSL (libcrypto/libssl) may be dynamic. libgcc_s is allowed because the server dynamically links it.
ALLOWED_DEPS_BASE='ld-linux.*\.so\.[0-9]+|libc\.so\.[0-9]+|libm\.so\.[0-9]+|libresolv\.so\.[0-9]+|libdl\.so\.[0-9]+|libpthread\.so\.[0-9]+|librt\.so\.[0-9]+|libcrypto\.so\.[0-9]+|libssl\.so\.[0-9]+|libgcc_s\.so\.[0-9]+|linux-vdso\.so\.[0-9]+'

# 2a) Check direct dependencies (DT_NEEDED) - stricter control
# Direct dependencies are what the extension explicitly links against.
ALLOWED_DIRECT_DEPS_REGEX="^(${ALLOWED_DEPS_BASE})$"

ext_dt_needed="$(dt_needed_libs "$EXT_SO")"
unexpected_direct_deps="$(echo "$ext_dt_needed" | grep -Ev "${ALLOWED_DIRECT_DEPS_REGEX}" || true)"
if [[ -n "$unexpected_direct_deps" ]]; then
    echo "Unexpected direct dynamic dependencies in extension (DT_NEEDED):"
    echo "$unexpected_direct_deps"
    die "Extension has unexpected direct dynamic library dependencies. Prefer static linking."
fi

# 2b) Check transitive dependencies (from ldd) - more lenient
# Transitive deps come from libraries that the extension links against.
# For example, if extension links OpenSSL, OpenSSL might pull in libz.
# libz is allowed transitively (via OpenSSL) but not as a direct dependency.
ALLOWED_TRANSITIVE_DEPS_REGEX="^(${ALLOWED_DEPS_BASE}|libz\.so\.[0-9]+)$"

unexpected_transitive_deps="$(echo "$ext_ldd" | grep -Ev "${ALLOWED_TRANSITIVE_DEPS_REGEX}" || true)"
if [[ -n "$unexpected_transitive_deps" ]]; then
    echo "Unexpected transitive dynamic dependencies in extension (from ldd):"
    echo "$unexpected_transitive_deps"
    die "Extension has unexpected transitive dynamic library dependencies."
fi

# 2c) Compare version conflicts.
# Normalize foo.so.3.1.2 -> foo.so.3 (works for lib* and ld-linux*, etc).
normalize_version() {
    sed -E 's/^(.+\.so\.[0-9]+)\..*$/\1/' | sort -u
}

srv_norm="$(echo "$srv_ldd" | normalize_version)"
ext_norm="$(echo "$ext_ldd" | normalize_version)"

# Same "base library" present in both is fine, but we want to catch cases where
# they appear with different major versions. We do a simple grouping:
#   base key: libfoo.so
#   version key: libfoo.so.X
#
# Build (libfoo.so -> set of majors) and fail if server and extension disagree.
base_and_major() {
    # Input: libfoo.so.X (or libfoo.so.X.Y already normalized)
    # Output: "libfoo.so libfoo.so.X"
    awk '{
    m=$0
    b=$0
    sub(/\.so\.[0-9]+.*/,".so", b)
    print b, m
  }'
}

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

echo "$srv_norm" | base_and_major | sort -u >"$tmpdir/srv.base_major"
echo "$ext_norm" | base_and_major | sort -u >"$tmpdir/ext.base_major"

# For each base library present in both, compare the major soname entry.
common_bases="$(comm -12 <(awk '{print $1}' "$tmpdir/srv.base_major" | sort -u) <(awk '{print $1}' "$tmpdir/ext.base_major" | sort -u) || true)"

conflicts=0
while read -r base; do
    [[ -z "$base" ]] && continue
    srv_major="$(awk -v b="$base" '$1==b {print $2}' "$tmpdir/srv.base_major" | sort -u | tr '\n' ' ')"
    ext_major="$(awk -v b="$base" '$1==b {print $2}' "$tmpdir/ext.base_major" | sort -u | tr '\n' ' ')"
    if [[ "$srv_major" != "$ext_major" ]]; then
        echo "Dependency major-version conflict for $base"
        echo "  server:   $srv_major"
        echo "  extension:$ext_major"
        conflicts=1
    fi
done <<<"$common_bases"

[[ "$conflicts" -eq 0 ]] || die "Found server/extension dependency conflicts (major soname mismatch)."

info "Dependency checks passed."

# ------------------------------------------------------------------------------
# 3) Symbol override hazard checks (nm)
#    Rule: find common symbols defined by both server and extension and reject them unless safe.
#
#    We also mandate allocator behavior:
#      - Extension must NOT define malloc/free/etc (otherwise it could override server allocator)
#      - Undefined references to allocator symbols are allowed (we only forbid definitions).
#
#    Allowlist:
#      - If there are legit shared symbols (e.g. some unwind symbols), add them to ALLOW_COMMON_REGEX.
# ------------------------------------------------------------------------------
info "3) Checking symbol override hazards (common defined symbols + allocator rules)"

# Symbols defined by each (dynamic only).
srv_defined="$(
    nm -D --defined-only "$SERVER_BIN" |
        awk '{print $NF}' |
        sed 's/@@.*$//' |
        sort -u
)"
ext_defined="$(
    nm -D --defined-only "$EXT_SO" |
        awk '{print $NF}' |
        sed 's/@@.*$//' |
        sort -u
)"

# Common defined symbols = potential override surface.
common_defined="$(comm -12 <(echo "$srv_defined") <(echo "$ext_defined") || true)"

# Narrow allowlist hook for known-safe shared symbols (start empty; tighten later).
# Example: ALLOW_COMMON_REGEX='^(_Unwind_|backtrace|dladdr)'
ALLOW_COMMON_REGEX='^$' # matches nothing

if [[ -n "$common_defined" ]]; then
    # Filter out allowlisted ones (if any).
    disallowed_common="$(echo "$common_defined" | grep -Ev "${ALLOW_COMMON_REGEX}" || true)"
    if [[ -n "$disallowed_common" ]]; then
        echo "Disallowed common defined dynamic symbols:"
        echo "$disallowed_common"
        die "Common defined symbols detected. This risks symbol interposition in either direction."
    fi
fi

# Allocator rules.
ALLOC_SYMS=(
    malloc free calloc realloc memalign posix_memalign aligned_alloc valloc pvalloc cfree malloc_usable_size
)

# 3a) Extension must NOT define allocator symbols.
ext_defined="$(
    nm -D --defined-only "$EXT_SO" |
        awk '{print $NF}' |
        sed 's/@@.*$//' |
        sed 's/@.*$//' |
        sort -u
)"
alloc_defined=""
for s in "${ALLOC_SYMS[@]}"; do
    if echo "$ext_defined" | grep -qx "$s"; then
        alloc_defined+="$s"$'\n'
    fi
done
if [[ -n "$alloc_defined" ]]; then
    echo "Extension defines allocator symbol(s) (not allowed):"
    echo "$alloc_defined"
    die "Extension must not provide allocator implementations. Avoid -Bsymbolic/RTLD_DEEPBIND and allocator providers."
fi

info "Symbol override checks passed."

info "All checks PASSED for extension: $EXT_SO"
