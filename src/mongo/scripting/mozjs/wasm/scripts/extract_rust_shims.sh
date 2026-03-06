#!/bin/bash
# Extracts the Rust encoding-shims static library from a SpiderMonkey WASI tarball.
#
# Required environment variables:
#   TARBALL  - Path to spidermonkey-wasip2-release.tar.gz
#   OUTPUT   - Output path for the extracted .a file
#
# Example standalone usage:
#   TARBALL=spidermonkey-wasip2-release.tar.gz \
#   OUTPUT=rust_shims.a \
#   bash scripts/extract_rust_shims.sh

set -euo pipefail
if [ -z "${TARBALL:-}" ]; then
    echo "ERROR: TARBALL not set." >&2
    exit 1
fi
if [ -z "${OUTPUT:-}" ]; then
    echo "ERROR: OUTPUT not set." >&2
    exit 1
fi

OUT_DIR="$(dirname "$OUTPUT")"
STAGE="$OUT_DIR/rust_stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"

# Extract lib directory from tarball (tarball has ./lib/ prefix).
tar -xzf "$TARBALL" -C "$STAGE" --strip-components=0 ./lib/libmongo_wasip2_rust_shims.a 2>/dev/null || {
    # Try without ./ prefix if that fails.
    if ! tar -xzf "$TARBALL" -C "$STAGE" lib/libmongo_wasip2_rust_shims.a 2>/dev/null; then
        echo "WARNING: Could not extract rust shims with either path prefix; will check below." >&2
    fi
}

RUST_SHIMS="$STAGE/lib/libmongo_wasip2_rust_shims.a"
if [ ! -f "$RUST_SHIMS" ]; then
    # Try alternative path.
    RUST_SHIMS="$STAGE/./lib/libmongo_wasip2_rust_shims.a"
fi

if [ -f "$RUST_SHIMS" ]; then
    cp "$RUST_SHIMS" "$OUTPUT"
    echo "Extracted rust shims to: $OUTPUT" >&2
    exit 0
fi

echo "ERROR: Rust shims not found in tarball" >&2
exit 1
