#!/usr/bin/env bash
#
# Verify all test extension .so files are compiled and linked correctly.
#
# Discovers mongod and all extension .so files, then runs verify_extension_visibility_test.sh on
# each one. Note that the following extensions intentionally do not pass the verifier because they
# omit one or both of the required load-protocol symbols:
#   - libno_symbol_bad_extension.so: lacks both phase 1 and phase 2 symbols.
#   - libno_get_extension_symbol_bad_extension.so: lacks the phase 2 symbol.
# We run dedicated negative tests on each to confirm the verifier catches them.
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERIFIER="$SCRIPT_DIR/verify_extension_visibility_test.sh"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

die() {
    echo "ERROR: $*" >&2
    exit 1
}

info() { echo "[verify_all_extensions] $*" >&2; }

if [[ "$(uname)" != "Linux" ]]; then
    info "Skipping: Linux-only"
    exit 0
fi

[[ -x "$VERIFIER" ]] || die "Verifier script not found or not executable: $VERIFIER"

# Locate mongod.
find_mongod() {
    if [[ -n "${MONGOD_PATH:-}" ]]; then
        echo "$MONGOD_PATH"
        return
    fi

    local candidates=(
        "$REPO_ROOT/dist-test/bin/mongod"
        "$REPO_ROOT/bazel-bin/install-dist-test/bin/mongod"
    )

    for c in "${candidates[@]}"; do
        if [[ -x "$c" ]]; then
            echo "$c"
            return
        fi
    done

    die "Could not find mongod. Build it first or set MONGOD_PATH."
}

# Locate extension directory.
find_ext_dir() {
    if [[ -n "${EXTENSION_DIR:-}" ]]; then
        echo "$EXTENSION_DIR"
        return
    fi

    local candidates=(
        "$REPO_ROOT/dist-test/lib"
        "$REPO_ROOT/bazel-bin/install-dist-test/lib"
        "$REPO_ROOT/bazel-bin/install-extensions/lib"
    )

    for c in "${candidates[@]}"; do
        if [[ -d "$c" ]]; then
            echo "$c"
            return
        fi
    done

    die "Could not find extension directory. Build extensions first or set EXTENSION_DIR."
}

MONGOD="$(find_mongod)"
EXT_DIR="$(find_ext_dir)"

info "mongod:        $MONGOD"
info "extension dir: $EXT_DIR"

# Collect all extension .so files except those expected to fail visibility (handled below as
# dedicated negative tests).
mapfile -t so_files < <(
    find "$EXT_DIR" -maxdepth 1 -name '*_extension*.so' \
        ! -name '*no_symbol_bad_extension*' \
        ! -name '*no_get_extension_symbol_bad_extension*' | sort
)

if [[ ${#so_files[@]} -eq 0 ]]; then
    die "No extension .so files found in $EXT_DIR"
fi

info "Found ${#so_files[@]} extension(s) to verify"

# Pre-compute server-side data once into temp files instead of per-extension.
cache_dir="$(mktemp -d)"
trap 'rm -rf "$cache_dir"' EXIT

info "Caching mongod symbol and dependency data..."
ldd "$MONGOD" |
    awk '
    $1 ~ /^\// {print $1}
    $2 == "=>" && $3 ~ /^\// {print $3}
  ' |
    xargs -r -n1 basename |
    sort -u >"$cache_dir/srv_ldd"

nm -D --defined-only "$MONGOD" |
    awk '{print $NF}' |
    sed 's/@@.*$//' |
    sort -u >"$cache_dir/srv_defined"
info "Cache ready."

export _CACHED_SRV_LDD_FILE="$cache_dir/srv_ldd"
export _CACHED_SRV_DEFINED_FILE="$cache_dir/srv_defined"

failures=()
for so in "${so_files[@]}"; do
    name="$(basename "$so")"
    info "Verifying: $name"
    if ! "$VERIFIER" "$MONGOD" "$so"; then
        failures+=("$name")
    fi
done

# Confirm that the bad-by-design extensions fail verification.
no_symbol_so="$EXT_DIR/libno_symbol_bad_extension.so"
if [[ -f "$no_symbol_so" ]]; then
    info "Negative test: libno_symbol_bad_extension.so should fail verification"
    if "$VERIFIER" "$MONGOD" "$no_symbol_so" &>/dev/null; then
        failures+=("libno_symbol_bad_extension.so (expected failure but passed)")
    fi
else
    info "libno_symbol_bad_extension.so not found in $EXT_DIR."
fi

no_get_extension_symbol_so="$EXT_DIR/libno_get_extension_symbol_bad_extension.so"
if [[ -f "$no_get_extension_symbol_so" ]]; then
    info "Negative test: libno_get_extension_symbol_bad_extension.so should fail verification"
    if "$VERIFIER" "$MONGOD" "$no_get_extension_symbol_so" &>/dev/null; then
        failures+=("libno_get_extension_symbol_bad_extension.so (expected failure but passed)")
    fi
else
    info "libno_get_extension_symbol_bad_extension.so not found in $EXT_DIR."
fi

echo
if [[ ${#failures[@]} -gt 0 ]]; then
    die "Verification FAILED for ${#failures[@]} extension(s): ${failures[*]}"
fi

info "All ${#so_files[@]} extension(s) passed verification."
