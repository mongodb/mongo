#!/usr/bin/env bash
# Build the cargo-bazel binary for ppc64le from the exact same rules_rust
# source used by the official releases, so that lock-file checksums are
# consistent across architectures.
#
# Run this on a ppc64le host.
#
# Prerequisites: curl, git, a C compiler (gcc).
# Rustup will be installed automatically if not already present.

set -euo pipefail

# ---------- configuration ---------------------------------------------------
# This MUST match the rules_rust version in MODULE.bazel.
RULES_RUST_TAG="${RULES_RUST_TAG:-0.69.0}"

# The official x86_64 release was compiled with rustc 1.93.1.  Using the same
# compiler version avoids any behavioural differences in dependency resolution.
RUSTC_VERSION="${RUSTC_VERSION:-1.93.1}"

OUTPUT_NAME="cargo-bazel-ppc64le"
OUT_DIR="$(pwd)"
# ---------------------------------------------------------------------------

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

echo "==> Working in $WORK_DIR"

# ---- 1. Ensure rustup + the required toolchain are available ---------------
if ! command -v rustup &>/dev/null; then
    echo "==> Installing rustup …"
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --default-toolchain none
    # shellcheck source=/dev/null
    source "$HOME/.cargo/env"
fi

echo "==> Installing rustc $RUSTC_VERSION …"
rustup install "$RUSTC_VERSION"
rustup default "$RUSTC_VERSION"

echo "==> Rust toolchain:"
rustc --version
cargo --version

# ---- 2. Clone the exact rules_rust tag ------------------------------------
echo "==> Cloning rules_rust @ $RULES_RUST_TAG …"
git clone --branch "$RULES_RUST_TAG" --depth 1 \
    https://github.com/bazelbuild/rules_rust "$WORK_DIR/rules_rust"

# ---- 3. Build cargo-bazel -------------------------------------------------
echo "==> Building cargo-bazel …"
cd "$WORK_DIR/rules_rust/crate_universe"
cargo build --release --locked --bin cargo-bazel

BINARY="$WORK_DIR/rules_rust/crate_universe/target/release/cargo-bazel"
if [[ ! -f "$BINARY" ]]; then
    echo "ERROR: binary not found at $BINARY" >&2
    exit 1
fi

echo "==> Built: $(file "$BINARY")"

cp "$BINARY" "$OUT_DIR/$OUTPUT_NAME"
echo "==> Output: $OUT_DIR/$OUTPUT_NAME"

echo "==> Done."
