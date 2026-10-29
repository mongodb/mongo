#!/usr/bin/env bash

set -euo pipefail

source_helper="${TEST_SRCDIR}/${TEST_WORKSPACE}/evergreen/prelude_python_windows.sh"
test_root="${TEST_TMPDIR}/prelude_python"
# Bazel supplies native drive paths on Windows. Convert before using the path
# in PATH, where a drive-letter colon would split the fake command directory.
if command -v cygpath >/dev/null 2>&1; then
    test_root=$(cygpath -u "$test_root")
fi
fake_bin="${test_root}/bin"
output_base="${test_root}/bazel-output"
repo_root="${test_root}/src"
mkdir -p "$fake_bin" "$output_base" "$repo_root/evergreen"

cat >"${fake_bin}/bazel" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

# Model Windows Bazelisk: tools/bazel.bat does not honor the Bash wrapper's
# MONGO_BAZEL_IN_HERMETIC_CONTAINER bypass. Skip it at the Bazelisk layer.
if [[ "${BAZELISK_SKIP_WRAPPER:-}" != 1 ]]; then
    echo "Windows Bazelisk would invoke the Python-dependent wrapper" >&2
    exit 1
fi

printf '%s\n' "$*" >>"$FAKE_BAZEL_LOG"

case "$*" in
    "info output_base")
        if [[ ! -e "$FAKE_BAZEL_INFO_MARKER" ]]; then
            touch "$FAKE_BAZEL_INFO_MARKER"
            exit 1
        fi
        printf '%s\n' "$FAKE_BAZEL_OUTPUT_BASE"
        ;;
    "build "*"@py_host//:all")
        python_path="$FAKE_BAZEL_OUTPUT_BASE/external/+setup_mongo_python_toolchains+py_host/dist/python.exe"
        mkdir -p "$(dirname "$python_path")"
        cp "$FAKE_PYTHON" "$python_path"
        chmod +x "$python_path"
        mkdir -p "$(dirname "$python_path")/DLLs" "$(dirname "$python_path")/Lib"
        printf '%s\n' 'fake DLL' >"$(dirname "$python_path")/DLLs/libcrypto.dll"
        printf '%s\n' 'fake standard library' >"$(dirname "$python_path")/Lib/os.py"
        ;;
    *)
        echo "unexpected Bazel invocation: $*" >&2
        exit 1
        ;;
esac
EOF
chmod +x "${fake_bin}/bazel"

cat >"${fake_bin}/python.exe" <<'EOF'
#!/usr/bin/env bash
if [[ "${1:-}" == "-c" ]]; then
    exit 0
fi
echo "unexpected fake Python invocation: $*" >&2
exit 1
EOF
chmod +x "${fake_bin}/python.exe"

export FAKE_BAZEL_LOG="${test_root}/bazel.log"
export FAKE_BAZEL_OUTPUT_BASE="$output_base"
export FAKE_BAZEL_INFO_MARKER="${test_root}/bazel-info-called"
export FAKE_PYTHON="${fake_bin}/python.exe"
: >"$FAKE_BAZEL_LOG"

# Stub path conversion on every host.
cygpath() {
    printf '%s\n' "$2"
}
export PATH="${fake_bin}:${PATH}"
unset BAZEL_REAL
unset BAZELISK_SKIP_WRAPPER
evergreen_dir="$repo_root/evergreen"
source "$source_helper"

# Explicitly pass a missing host interpreter, even on healthy Windows hosts.
selected_python=$(_windows_python "$test_root/missing-host/python.exe")

expected_python="$repo_root/.tmp/evergreen-python/python.exe"
if [[ "$selected_python" != "$expected_python" ]]; then
    echo "expected hermetic Python '$expected_python', got '$selected_python'" >&2
    exit 1
fi

grep -q -- "@py_host//:all" "$FAKE_BAZEL_LOG"
[[ "$(wc -l <"$FAKE_BAZEL_LOG")" -eq 3 ]]
cmp "$output_base/external/+setup_mongo_python_toolchains+py_host/dist/DLLs/libcrypto.dll" \
    "$repo_root/.tmp/evergreen-python/DLLs/libcrypto.dll"
cmp "$output_base/external/+setup_mongo_python_toolchains+py_host/dist/Lib/os.py" \
    "$repo_root/.tmp/evergreen-python/Lib/os.py"

# Changing or clearing the Bazel output base must not invalidate the runtime.
mv "$output_base" "$test_root/old-output-base"
: >"$FAKE_BAZEL_LOG"
[[ "$(_windows_python "$test_root/missing-host/python.exe")" == "$expected_python" ]]
[[ ! -s "$FAKE_BAZEL_LOG" ]]
"$selected_python" -c 'raise SystemExit(0)'

# A healthy host interpreter takes precedence and does not invoke Bazel.
[[ "$(_windows_python "$FAKE_PYTHON")" == "$FAKE_PYTHON" ]]
[[ ! -s "$FAKE_BAZEL_LOG" ]]

# A failed copy can leave a working executable without the rest of the runtime.
# The next selection must finish copying rather than reuse that partial tree.
evergreen_dir="$test_root/interrupted/src/evergreen"
mkdir -p "$evergreen_dir"
interrupted_runtime="$test_root/interrupted/src/.tmp/evergreen-python"
cp() {
    if [[ "$1" == '-R' && "${3:-}" == "$interrupted_runtime/" && ! -f "$test_root/copy-interrupted" ]]; then
        touch "$test_root/copy-interrupted"
        command cp "$FAKE_PYTHON" "$interrupted_runtime/python.exe"
        return 1
    fi
    command cp "$@"
}
if _windows_python "$test_root/missing-host/python.exe"; then
    echo 'Expected the interrupted runtime copy to fail' >&2
    exit 1
fi
_python_works "$interrupted_runtime/python.exe"
[[ ! -e "$interrupted_runtime/DLLs/libcrypto.dll" ]]
[[ "$(_windows_python "$test_root/missing-host/python.exe")" == "$interrupted_runtime/python.exe" ]]
cmp "$output_base/external/+setup_mongo_python_toolchains+py_host/dist/DLLs/libcrypto.dll" \
    "$interrupted_runtime/DLLs/libcrypto.dll"
cmp "$output_base/external/+setup_mongo_python_toolchains+py_host/dist/Lib/os.py" \
    "$interrupted_runtime/Lib/os.py"
unset -f cp
