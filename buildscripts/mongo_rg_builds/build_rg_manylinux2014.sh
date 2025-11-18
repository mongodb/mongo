#!/usr/bin/env bash
set -euo pipefail

# -------- config (overridable via env) --------------------------------------
ARCH="${ARCH:-$(uname -m)}" # x86_64 | aarch64 | s390x | ppc64le
RG_REPO="${RG_REPO:-https://github.com/BurntSushi/ripgrep.git}"
RG_REF="${RG_REF:-master}" # tag like 14.1.0 or a commit
OUT_DIR="${OUT_DIR:-$(pwd)/dist}"
PLATFORM="${PLATFORM:-}"         # e.g. linux/arm64 if you want to force
DOCKER_IMAGE=""                  # filled below
CPU_BASELINE="${CPU_BASELINE:-}" # default per-arch below

# Map arch -> image + defaults
case "$ARCH" in
x86_64 | amd64)
    ARCH=x86_64
    DOCKER_IMAGE="quay.io/pypa/manylinux2014_x86_64"
    CPU_BASELINE="${CPU_BASELINE:-x86-64}" # or x86-64-v2 / v3
    ;;
aarch64 | arm64)
    ARCH=aarch64
    DOCKER_IMAGE="quay.io/pypa/manylinux2014_aarch64"
    CPU_BASELINE="${CPU_BASELINE:-generic}"
    ;;
s390x | 390x)
    ARCH=s390x
    DOCKER_IMAGE="quay.io/pypa/manylinux2014_s390x"
    CPU_BASELINE="${CPU_BASELINE:-generic}"
    ;;
ppc64le | ppc)
    ARCH=ppc64le
    DOCKER_IMAGE="quay.io/pypa/manylinux2014_ppc64le"
    CPU_BASELINE="${CPU_BASELINE:-generic}"
    ;;
*)
    echo "Unsupported ARCH='$ARCH'. Expected x86_64|aarch64|s390x|ppc64le." >&2
    exit 1
    ;;
esac

mkdir -p "$OUT_DIR"

echo "==> Build ripgrep for manylinux2014 ($ARCH)"
echo "    Image: $DOCKER_IMAGE"
echo "    CPU_BASELINE: $CPU_BASELINE"
[ -n "$PLATFORM" ] && echo "    docker --platform: $PLATFORM"

# Compose optional --platform flag
PLATFORM_ARGS=()
[ -n "$PLATFORM" ] && PLATFORM_ARGS=(--platform "$PLATFORM")

docker run --rm -t "${PLATFORM_ARGS[@]}" \
    -v "$OUT_DIR":/out \
    "$DOCKER_IMAGE" \
    bash -lc '
    set -euo pipefail

    echo "==> glibc baseline:"
    ldd --version > /tmp/lddv && head -1 /tmp/lddv

    # Enable newer GCC if available; guard against nounset
    if [ -f /opt/rh/devtoolset-10/enable ]; then
        set +u
        # shellcheck disable=SC1091
        source /opt/rh/devtoolset-10/enable
        set -u
        echo "Using devtoolset-10"
    fi

    yum -y install git cmake make which pkgconfig curl perl binutils >/dev/null 2>&1 || true

    echo "==> Install Rust (minimal profile)"
    curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
    # shellcheck disable=SC1091
    source "$HOME/.cargo/env"
    rustc -V; cargo -V

    echo "==> Clone ripgrep ('"$RG_REF"')"
    rm -rf /tmp/ripgrep
    git clone --depth=1 --branch '"$RG_REF"' '"$RG_REPO"' /tmp/ripgrep
    cd /tmp/ripgrep

    echo "==> Build with bundled+static PCRE2, LTO, single CGU, conservative CPU"
    export PCRE2_SYS_BUNDLED=1
    export PCRE2_SYS_STATIC=1
    export CARGO_PROFILE_RELEASE_LTO=fat
    export CARGO_PROFILE_RELEASE_CODEGEN_UNITS=1
    export CARGO_PROFILE_RELEASE_PANIC=abort
    export RUSTFLAGS="-C target-cpu='"$CPU_BASELINE"' -C strip=symbols"

    cargo clean
    cargo build --release --features pcre2

    BIN=/tmp/ripgrep/target/release/rg
    strip "$BIN" || true

    echo "==> GLIBC symbols used:"
    objdump -T "$BIN" | grep -o "GLIBC_[0-9]\\+\\.[0-9]\\+" | sort -u || true

    OUT_NAME=rg-manylinux2014-'"$ARCH"'
    cp "$BIN" /out/$OUT_NAME
    echo "==> Wrote /out/$OUT_NAME"
    '

echo "Built: $OUT_DIR/rg-manylinux2014-$ARCH"
