#!/usr/bin/env bash
set -euo pipefail

RG_REPO="${RG_REPO:-https://github.com/BurntSushi/ripgrep.git}"
RG_REF="${RG_REF:-master}"
OUT_DIR="${OUT_DIR:-$(pwd)/dist}"
DEPLOY_X86="${DEPLOY_X86:-10.13}"
DEPLOY_ARM="${DEPLOY_ARM:-11.0}"
CPU_BASE_X86="${CPU_BASE_X86:-x86-64}"
CPU_BASE_ARM="${CPU_BASE_ARM:-generic}"

mkdir -p "$OUT_DIR"

# toolchain
if ! command -v rustup >/dev/null 2>&1; then
    curl -sSf https://sh.rustup.rs | sh -s -- -y
    . "$HOME/.cargo/env"
fi
rustup target add x86_64-apple-darwin aarch64-apple-darwin

# repo
test -d ripgrep || git clone --depth=1 --branch "$RG_REF" "$RG_REPO" ripgrep
cd ripgrep

# shared knobs
export PCRE2_SYS_BUNDLED=1
export PCRE2_SYS_STATIC=1
export CARGO_PROFILE_RELEASE_LTO=fat
export CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1
export CARGO_PROFILE_RELEASE_PANIC=abort

SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
CC_BIN="$(xcrun -f clang)"

# --- x86_64 ---
echo "==> x86_64 (min macOS $DEPLOY_X86)"
export CARGO_TARGET_DIR="$(pwd)/target-x86_64"
rm -rf "$CARGO_TARGET_DIR"
env SDKROOT="$SDKROOT" CC="$CC_BIN" CFLAGS="-mmacosx-version-min=$DEPLOY_X86" \
    MACOSX_DEPLOYMENT_TARGET="$DEPLOY_X86" \
    RUSTFLAGS="-C target-cpu=$CPU_BASE_X86 -C strip=symbols -C link-arg=-mmacosx-version-min=$DEPLOY_X86" \
    cargo build --release --features pcre2 --target x86_64-apple-darwin
RG_X86="$CARGO_TARGET_DIR/x86_64-apple-darwin/release/rg"

# --- arm64 ---
echo "==> arm64 (min macOS $DEPLOY_ARM)"
export CARGO_TARGET_DIR="$(pwd)/target-aarch64"
rm -rf "$CARGO_TARGET_DIR"
env SDKROOT="$SDKROOT" CC="$CC_BIN" CFLAGS="-mmacosx-version-min=$DEPLOY_ARM" \
    MACOSX_DEPLOYMENT_TARGET="$DEPLOY_ARM" \
    RUSTFLAGS="-C target-cpu=$CPU_BASE_ARM -C strip=symbols -C link-arg=-Wl,-platform_version,macos,${DEPLOY_ARM},${DEPLOY_ARM}" \
    cargo build --release --features pcre2 --target aarch64-apple-darwin
RG_ARM="$CARGO_TARGET_DIR/aarch64-apple-darwin/release/rg"

# sanity
[ -f "$RG_X86" ] || {
    echo "missing $RG_X86"
    exit 1
}
[ -f "$RG_ARM" ] || {
    echo "missing $RG_ARM"
    exit 1
}

# --- universal2 ---
OUT="$OUT_DIR/rg-macos-universal2"
lipo -create -output "$OUT" "$RG_X86" "$RG_ARM"
strip -S "$OUT" || true

echo "==> Wrote $OUT"
file "$OUT"
otool -L "$OUT"
