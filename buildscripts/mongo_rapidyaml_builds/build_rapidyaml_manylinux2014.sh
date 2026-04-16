#!/usr/bin/env bash
set -euo pipefail

# -------- config (overridable via env) --------------------------------------
ARCH="${ARCH:-$(uname -m)}" # x86_64 | aarch64 | s390x | ppc64le
RAPIDYAML_REPO="${RAPIDYAML_REPO:-https://github.com/mongodb-forks/rapidyaml.git}"
RAPIDYAML_REF="${RAPIDYAML_REF:-a5d485fd44719e1c03e059177fc1f695fc462b66}"
RAPIDYAML_VERSION="${RAPIDYAML_VERSION:-}"
PYTHON_TAG="${PYTHON_TAG:-cp313-cp313}"
OUT_DIR="${OUT_DIR:-$(pwd)/dist}"
PLATFORM="${PLATFORM:-}" # e.g. linux/arm64 if you want to force Docker
CPU_FLAGS="${CPU_FLAGS:-}"
DOCKER_IMAGE=""
AUDITWHEEL_PLAT=""

if [[ -z "$RAPIDYAML_VERSION" ]]; then
    echo "RAPIDYAML_VERSION must be set (for example: 0.9.0.post0)." >&2
    exit 1
fi

case "$ARCH" in
x86_64 | amd64)
    ARCH=x86_64
    DOCKER_IMAGE="quay.io/pypa/manylinux2014_x86_64"
    AUDITWHEEL_PLAT="manylinux2014_x86_64"
    CPU_FLAGS="${CPU_FLAGS:--march=x86-64 -mtune=generic}"
    ;;
aarch64 | arm64)
    ARCH=aarch64
    DOCKER_IMAGE="quay.io/pypa/manylinux2014_aarch64"
    AUDITWHEEL_PLAT="manylinux2014_aarch64"
    CPU_FLAGS="${CPU_FLAGS:--mcpu=generic}"
    ;;
s390x | 390x)
    ARCH=s390x
    DOCKER_IMAGE="quay.io/pypa/manylinux2014_s390x"
    AUDITWHEEL_PLAT="manylinux2014_s390x"
    ;;
ppc64le | ppc)
    ARCH=ppc64le
    DOCKER_IMAGE="quay.io/pypa/manylinux2014_ppc64le"
    AUDITWHEEL_PLAT="manylinux2014_ppc64le"
    ;;
*)
    echo "Unsupported ARCH='$ARCH'. Expected x86_64|aarch64|s390x|ppc64le." >&2
    exit 1
    ;;
esac

mkdir -p "$OUT_DIR"

echo "==> Build rapidyaml wheel for manylinux2014 ($ARCH)"
echo "    RAPIDYAML_REF=$RAPIDYAML_REF"
echo "    RAPIDYAML_VERSION=$RAPIDYAML_VERSION"
echo "    PYTHON_TAG=$PYTHON_TAG"
echo "    Image: $DOCKER_IMAGE"
[ -n "$PLATFORM" ] && echo "    docker --platform: $PLATFORM"
[ -n "$CPU_FLAGS" ] && echo "    CPU_FLAGS=$CPU_FLAGS"

PLATFORM_ARGS=()
[ -n "$PLATFORM" ] && PLATFORM_ARGS=(--platform "$PLATFORM")

docker run --rm -t "${PLATFORM_ARGS[@]}" \
    -e RAPIDYAML_REPO="$RAPIDYAML_REPO" \
    -e RAPIDYAML_REF="$RAPIDYAML_REF" \
    -e RAPIDYAML_VERSION="$RAPIDYAML_VERSION" \
    -e PYTHON_TAG="$PYTHON_TAG" \
    -e AUDITWHEEL_PLAT="$AUDITWHEEL_PLAT" \
    -e CPU_FLAGS="$CPU_FLAGS" \
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
    ldd --version > /tmp/lddv && sed -n "1p" /tmp/lddv

    yum -y install git >/dev/null 2>&1 || true

    rm -rf /tmp/rapidyaml /tmp/wheelhouse /tmp/repaired
    git clone "$RAPIDYAML_REPO" /tmp/rapidyaml
    cd /tmp/rapidyaml
    git -c advice.detachedHead=false checkout "$RAPIDYAML_REF"
    git submodule update --init --recursive

    export SETUPTOOLS_SCM_PRETEND_VERSION="$RAPIDYAML_VERSION"
    export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(nproc)}"
    if [[ -n "${CPU_FLAGS}" ]]; then
        export CFLAGS="${CFLAGS:+$CFLAGS }${CPU_FLAGS}"
        export CXXFLAGS="${CXXFLAGS:+$CXXFLAGS }${CPU_FLAGS}"
    fi

    "$PYBIN" -m pip install --upgrade pip build auditwheel
    "$PYBIN" -m build --wheel --outdir /tmp/wheelhouse

    wheel="$(ls -1 /tmp/wheelhouse/*.whl)"
    echo "==> Built wheel: $(basename "$wheel")"
    auditwheel show "$wheel"

    mkdir -p /tmp/repaired
    auditwheel repair --plat "$AUDITWHEEL_PLAT" --wheel-dir /tmp/repaired "$wheel"

    repaired_wheel="$(ls -1 /tmp/repaired/*.whl)"
    cp "$repaired_wheel" /out/
    echo "==> Wrote /out/$(basename "$repaired_wheel")"

    "$PYBIN" -m pip install --force-reinstall "$repaired_wheel"
    "$PYBIN" - <<'"'"'PY'"'"'
import ryml
print("Imported ryml from", ryml.__file__)
PY

    sha256sum "$repaired_wheel"
    '

echo "Built wheels in: $OUT_DIR"
