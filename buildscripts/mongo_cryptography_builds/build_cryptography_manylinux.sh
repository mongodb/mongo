#!/usr/bin/env bash
set -euo pipefail

# Build a manylinux_2_28 wheel of the `cryptography` PyPI package for archs
# that the upstream project does not publish wheels for on PyPI — namely
# s390x and ppc64le. The wheels we produce here are uploaded to
# `s3://mdb-build-public/cryptography_wheels/simple/` and consumed by
# `pip.parse` via the `[[tool.uv.index]]` + `[tool.uv.sources]` entries in
# the repo's top-level `pyproject.toml`.
#
# Motivation: cryptography 35+ ships a Rust extension (via maturin /
# setuptools-rust). On archs where PyPI doesn't carry a binary wheel, pip
# falls through to the sdist build, which requires a working Rust toolchain
# (1.65+) inside the build sandbox. `pip.parse` runs the wheel build in a
# repository_rule, where Rust isn't available, so the sdist build fails
# (`error: rustc not found` or `toolchain 'stable-<arch>-unknown-linux-gnu'
# is not installed`). Pre-building the wheel here, in a container that
# already has Rust + OpenSSL, sidesteps the problem the same way that
# `mongo_rapidyaml_builds/` does for rapidyaml.
#
# cryptography ships abi3 wheels (`cp37-abi3-…`), so the wheel built by a
# cp313 interpreter works for every Python 3.7+ consumer; only one wheel
# per arch is needed.
#
# Image choice: we use `manylinux_2_28_<arch>` (AlmaLinux 8 base, dnf, glibc
# 2.28+) rather than the older `manylinux2014_<arch>` (CentOS 7 base,
# clefos repo). Reasons:
#
#   * `manylinux2014_s390x` and `manylinux2014_ppc64le` don't ship a
#     pre-built OpenSSL — the pypa CI infra punts on it for archs they
#     can't cross-build cleanly — and the `clefos-rh` yum mirror used to
#     install it on the fly (mirrors.sinenomine.net) is frequently
#     unreachable. The `openssl-sys` crate then has nowhere to find
#     OpenSSL and the build errors out.
#   * `manylinux_2_28` is what cryptography itself uses upstream for the
#     `manylinux_2_28_{x86_64,aarch64,armv7l}` wheels on PyPI, so this
#     mirrors their build environment.
#   * The rhel83-zseries-small / rhel81-power8-small CI distros (and
#     rhel9-power, rhel9-zseries) all have glibc >= 2.28, so the resulting
#     wheel is consumable.

# -------- config (overridable via env) --------------------------------------
ARCH="${ARCH:-$(uname -m)}" # x86_64 | aarch64 | s390x | ppc64le
CRYPTOGRAPHY_VERSION="${CRYPTOGRAPHY_VERSION:-}"
RUST_VERSION="${RUST_VERSION:-1.74.0}"  # cryptography 44.x requires >= 1.65; pin for reproducibility.
PYTHON_TAG="${PYTHON_TAG:-cp313-cp313}" # The interpreter used to *invoke* the build; the wheel itself is abi3.
OUT_DIR="${OUT_DIR:-$(pwd)/dist}"
PLATFORM="${PLATFORM:-}" # e.g. linux/s390x to force Docker through QEMU
DOCKER_IMAGE=""
AUDITWHEEL_PLAT=""

if [[ -z "$CRYPTOGRAPHY_VERSION" ]]; then
    echo "CRYPTOGRAPHY_VERSION must be set (for example: 44.0.2)." >&2
    echo "It must match the version pinned in pyproject.toml's 'platform' dep group." >&2
    exit 1
fi

case "$ARCH" in
x86_64 | amd64)
    ARCH=x86_64
    DOCKER_IMAGE="quay.io/pypa/manylinux_2_28_x86_64"
    AUDITWHEEL_PLAT="manylinux_2_28_x86_64"
    ;;
aarch64 | arm64)
    ARCH=aarch64
    DOCKER_IMAGE="quay.io/pypa/manylinux_2_28_aarch64"
    AUDITWHEEL_PLAT="manylinux_2_28_aarch64"
    ;;
s390x | 390x)
    ARCH=s390x
    DOCKER_IMAGE="quay.io/pypa/manylinux_2_28_s390x"
    AUDITWHEEL_PLAT="manylinux_2_28_s390x"
    ;;
ppc64le | ppc)
    ARCH=ppc64le
    DOCKER_IMAGE="quay.io/pypa/manylinux_2_28_ppc64le"
    AUDITWHEEL_PLAT="manylinux_2_28_ppc64le"
    ;;
*)
    echo "Unsupported ARCH='$ARCH'. Expected x86_64|aarch64|s390x|ppc64le." >&2
    exit 1
    ;;
esac

mkdir -p "$OUT_DIR"

echo "==> Build cryptography wheel for manylinux_2_28 ($ARCH)"
echo "    CRYPTOGRAPHY_VERSION=$CRYPTOGRAPHY_VERSION"
echo "    RUST_VERSION=$RUST_VERSION"
echo "    PYTHON_TAG=$PYTHON_TAG"
echo "    Image: $DOCKER_IMAGE"
[ -n "$PLATFORM" ] && echo "    docker --platform: $PLATFORM"

PLATFORM_ARGS=()
[ -n "$PLATFORM" ] && PLATFORM_ARGS=(--platform "$PLATFORM")

docker run --rm -t "${PLATFORM_ARGS[@]}" \
    -e CRYPTOGRAPHY_VERSION="$CRYPTOGRAPHY_VERSION" \
    -e RUST_VERSION="$RUST_VERSION" \
    -e PYTHON_TAG="$PYTHON_TAG" \
    -e AUDITWHEEL_PLAT="$AUDITWHEEL_PLAT" \
    -e ARCH="$ARCH" \
    -v "$OUT_DIR":/out \
    "$DOCKER_IMAGE" \
    bash -lc '
    set -euo pipefail

    PYBIN="/opt/python/${PYTHON_TAG}/bin/python"
    if [[ ! -x "$PYBIN" ]]; then
        echo "Python interpreter not found: $PYBIN" >&2
        exit 1
    fi

    echo "==> glibc baseline:"
    ldd --version | sed -n "1p"

    # OpenSSL + libffi headers: required by the `openssl-sys` and `cffi`
    # build steps inside cryptography. AlmaLinux 8 ships these in the
    # `appstream` / `baseos` repos which are configured by default in the
    # manylinux_2_28 images. dnf is the package manager (yum is aliased
    # to dnf on RHEL 8 derivatives).
    echo "==> Installing openssl-devel + libffi-devel via dnf..."
    dnf -y install openssl-devel libffi-devel pkgconfig
    pkg-config --modversion openssl
    echo "    OpenSSL pc file: $(pkg-config --variable=pcfiledir openssl)/openssl.pc"

    # cryptography 44.x needs Rust >= 1.65. The pypa/manylinux_2_28_<arch>
    # images do not ship a Rust toolchain; install rustup with an explicit
    # version pin for reproducibility, then arrange for cargo and rustc to
    # live on PATH for the build step.
    echo "==> Installing Rust ${RUST_VERSION} for ${ARCH}..."
    export CARGO_HOME=/root/.cargo
    export RUSTUP_HOME=/root/.rustup
    curl --proto "=https" --tlsv1.2 -sSf https://sh.rustup.rs |
        sh -s -- -y --default-toolchain "${RUST_VERSION}" --profile minimal --no-modify-path
    export PATH="${CARGO_HOME}/bin:${PATH}"
    rustc --version
    cargo --version

    rm -rf /tmp/wheelhouse /tmp/repaired
    mkdir -p /tmp/wheelhouse /tmp/repaired

    # `pip wheel` with `--no-binary cryptography` forces a build from
    # sdist (skipping any wheel on PyPI), which is what we want for s390x
    # / ppc64le since none exist there. For x86_64 / aarch64 the same
    # invocation works (it just compiles instead of downloading), so this
    # script is uniform across archs.
    "$PYBIN" -m pip install --upgrade pip auditwheel
    "$PYBIN" -m pip wheel \
        --no-binary cryptography \
        --no-deps \
        -w /tmp/wheelhouse \
        "cryptography==${CRYPTOGRAPHY_VERSION}"

    wheel="$(ls -1 /tmp/wheelhouse/cryptography-*.whl)"
    echo "==> Built wheel: $(basename "$wheel")"
    auditwheel show "$wheel"

    # auditwheel repair vendors the OpenSSL and libffi shared libs into
    # the wheel so the resulting artifact runs on the manylinux_2_28
    # baseline without external runtime deps. Skips if the wheel is
    # already in compliance.
    auditwheel repair --plat "$AUDITWHEEL_PLAT" --wheel-dir /tmp/repaired "$wheel" ||
        cp "$wheel" /tmp/repaired/

    repaired_wheel="$(ls -1 /tmp/repaired/cryptography-*.whl)"
    cp "$repaired_wheel" /out/
    echo "==> Wrote /out/$(basename "$repaired_wheel")"

    # Smoke test: install into a fresh venv and import. The fresh venv
    # avoids picking up the `cryptography` egg that pip-wheel'"'"'s build
    # backend installed alongside the .whl earlier.
    "$PYBIN" -m venv /tmp/smokeenv
    /tmp/smokeenv/bin/pip install --force-reinstall "$repaired_wheel"
    /tmp/smokeenv/bin/python -c "
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
import cryptography
print(\"cryptography\", cryptography.__version__, \"OK on\", \"'"${ARCH}"'\")
"

    sha256sum "$repaired_wheel"
    '

echo "Built wheels in: $OUT_DIR"
