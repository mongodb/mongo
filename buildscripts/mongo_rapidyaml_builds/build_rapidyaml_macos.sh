#!/usr/bin/env bash
set -euo pipefail

RAPIDYAML_REPO="${RAPIDYAML_REPO:-https://github.com/mongodb-forks/rapidyaml.git}"
RAPIDYAML_REF="${RAPIDYAML_REF:-a5d485fd44719e1c03e059177fc1f695fc462b66}"
RAPIDYAML_VERSION="${RAPIDYAML_VERSION:-}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
OUT_DIR="${OUT_DIR:-$(pwd)/dist}"
ARCH="${ARCH:-$(uname -m)}"
HOST_ARCH="$(uname -m)"
CPU_FLAGS="${CPU_FLAGS:-}"

if [[ -z "$RAPIDYAML_VERSION" ]]; then
    echo "RAPIDYAML_VERSION must be set (for example: 0.9.0.post0)." >&2
    exit 1
fi

case "$HOST_ARCH" in
x86_64 | amd64)
    HOST_ARCH=x86_64
    ;;
arm64 | aarch64)
    HOST_ARCH=arm64
    ;;
*)
    echo "Unsupported host arch '$HOST_ARCH'. Expected x86_64 or arm64." >&2
    exit 1
    ;;
esac

case "$ARCH" in
x86_64 | amd64)
    ARCH=x86_64
    MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-10.13}"
    CPU_FLAGS="${CPU_FLAGS:--march=x86-64 -mtune=generic}"
    ;;
arm64 | aarch64)
    ARCH=arm64
    MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
    ;;
*)
    echo "Unsupported ARCH='$ARCH'. Expected x86_64 or arm64." >&2
    exit 1
    ;;
esac

if [[ "$ARCH" != "$HOST_ARCH" ]]; then
    echo "build_rapidyaml_macos.sh must run natively on the target macOS arch." >&2
    echo "Requested ARCH='$ARCH', but host is '$HOST_ARCH'." >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/rapidyaml-build.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT
VENV_DIR="$TMP_DIR/venv"
ENV_PYTHON="$VENV_DIR/bin/python"
BUILD_REQUIREMENTS_FILE="$TMP_DIR/build-requirements.txt"

SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
COMMON_CFLAGS="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
COMMON_LDFLAGS="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET"
ARCHFLAGS_VALUE=""
CMAKE_OSX_ARCHITECTURES_VALUE=""
CMAKE_FLAGS_VALUE="${CMAKE_FLAGS:-}"

if [[ "$ARCH" == "x86_64" ]]; then
    COMMON_CFLAGS="-arch $ARCH $COMMON_CFLAGS"
    COMMON_LDFLAGS="-arch $ARCH $COMMON_LDFLAGS"
    ARCHFLAGS_VALUE="-arch $ARCH"
    CMAKE_OSX_ARCHITECTURES_VALUE="$ARCH"
else
    # c4core's older TargetArchitecture.cmake rejects explicit "arm64" on macOS.
    # Build natively on Apple Silicon and tell the project logic to treat the CPU as aarch64.
    CMAKE_FLAGS_VALUE="${CMAKE_FLAGS_VALUE:+$CMAKE_FLAGS_VALUE }-DCMAKE_SYSTEM_PROCESSOR=aarch64"
fi

if [[ -n "$CPU_FLAGS" ]]; then
    COMMON_CFLAGS="$COMMON_CFLAGS $CPU_FLAGS"
fi

echo "==> Build rapidyaml wheel for macOS ($ARCH)"
echo "    RAPIDYAML_REF=$RAPIDYAML_REF"
echo "    RAPIDYAML_VERSION=$RAPIDYAML_VERSION"
echo "    PYTHON_BIN=$PYTHON_BIN"
echo "    MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET"
[ -n "$CPU_FLAGS" ] && echo "    CPU_FLAGS=$CPU_FLAGS"
[ -n "$CMAKE_FLAGS_VALUE" ] && echo "    CMAKE_FLAGS=$CMAKE_FLAGS_VALUE"

git clone "$RAPIDYAML_REPO" "$TMP_DIR/rapidyaml"
cd "$TMP_DIR/rapidyaml"
git -c advice.detachedHead=false checkout "$RAPIDYAML_REF"
git submodule update --init --recursive

"$PYTHON_BIN" -m venv "$VENV_DIR"
if [[ ! -x "$ENV_PYTHON" ]]; then
    echo "Failed to create virtualenv at $VENV_DIR" >&2
    exit 1
fi

export SETUPTOOLS_SCM_PRETEND_VERSION="$RAPIDYAML_VERSION"
export SDKROOT
if [[ -n "$ARCHFLAGS_VALUE" ]]; then
    export ARCHFLAGS="$ARCHFLAGS_VALUE"
else
    unset ARCHFLAGS || true
fi
if [[ -n "$CMAKE_OSX_ARCHITECTURES_VALUE" ]]; then
    export CMAKE_OSX_ARCHITECTURES="$CMAKE_OSX_ARCHITECTURES_VALUE"
else
    unset CMAKE_OSX_ARCHITECTURES || true
fi
export CMAKE_FLAGS="$CMAKE_FLAGS_VALUE"
export MACOSX_DEPLOYMENT_TARGET
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-$(sysctl -n hw.logicalcpu)}"
export CFLAGS="${CFLAGS:+$CFLAGS }$COMMON_CFLAGS"
export CXXFLAGS="${CXXFLAGS:+$CXXFLAGS }$COMMON_CFLAGS"
export LDFLAGS="${LDFLAGS:+$LDFLAGS }$COMMON_LDFLAGS"
export PATH="$VENV_DIR/bin:$PATH"

"$ENV_PYTHON" -m pip install --upgrade "pip<26" setuptools wheel build delocate "packaging<26"

"$ENV_PYTHON" - "$BUILD_REQUIREMENTS_FILE" <<'PY'
import pathlib
import sys
import tomllib


def normalize_requirement(req_string: str) -> str:
    requirement, sep, marker = req_string.partition(";")
    requirement = requirement.strip()

    if "~=" in requirement:
        name, version = requirement.split("~=", 1)
        requirement = f"{name.strip()}=={version.strip()}"

    if sep:
        return f"{requirement}; {marker.strip()}"
    return requirement


data = tomllib.loads(pathlib.Path("pyproject.toml").read_text(encoding="utf-8"))
pathlib.Path(sys.argv[1]).write_text(
    "".join(f"{normalize_requirement(req)}\n" for req in data["build-system"]["requires"]),
    encoding="utf-8",
)
PY

while IFS= read -r requirement; do
    [[ -n "$requirement" ]] || continue
    echo "==> Installing build dependency $requirement"
    "$ENV_PYTHON" -m pip install --upgrade "$requirement"
done <"$BUILD_REQUIREMENTS_FILE"

SWIG_EXECUTABLE="$(command -v swig || true)"
if [[ -z "$SWIG_EXECUTABLE" ]]; then
    echo "swig not found after installing build dependencies." >&2
    exit 1
fi
export SWIG_EXECUTABLE
export SWIG_DIR="$("$SWIG_EXECUTABLE" -swiglib)"
if [[ -z "$SWIG_DIR" ]]; then
    echo "Failed to resolve SWIG_DIR from $SWIG_EXECUTABLE" >&2
    exit 1
fi

"$ENV_PYTHON" -m build --wheel --no-isolation --outdir "$TMP_DIR/wheelhouse"

wheel="$(ls -1 "$TMP_DIR"/wheelhouse/*.whl)"
echo "==> Built wheel: $(basename "$wheel")"
delocate-listdeps "$wheel"

mkdir -p "$TMP_DIR/repaired"
# Do not vendor the interpreter's Python.framework into the wheel. The
# extension should load libpython from the target Python installation.
delocate-wheel --exclude "Python.framework" -w "$TMP_DIR/repaired" -v "$wheel"

repaired_wheel="$(ls -1 "$TMP_DIR"/repaired/*.whl)"
cp "$repaired_wheel" "$OUT_DIR/"
echo "==> Wrote $OUT_DIR/$(basename "$repaired_wheel")"

"$ENV_PYTHON" -m pip install --force-reinstall "$repaired_wheel"
"$ENV_PYTHON" - <<'PY'
import ryml
print("Imported ryml from", ryml.__file__)
PY

shasum -a 256 "$repaired_wheel"
