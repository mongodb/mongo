#!/usr/bin/env bash

set -o errexit
set -o nounset
set -o pipefail

source_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ ! -f "$source_root/tools/bazel.bat" ]]; then
    echo "Windows Bazel wrapper is missing from the test runfiles" >&2
    exit 1
fi
if grep -Eq \
    'run_hermetic_container_or_bazel|hermetic_container_integration\.py|MONGO_BAZEL_USE_HERMETIC_CONTAINER' \
    "$source_root/tools/bazel.bat"; then
    echo "Windows Bazel wrapper must invoke Bazel natively" >&2
    exit 1
fi

test_root="$(mktemp -d)"
trap 'rm -rf "$test_root"' EXIT

repo_root="$test_root/repo"
fake_bin="$test_root/bin"
mkdir -p "$repo_root/tools" "$repo_root/bazel/wrapper_hook" "$fake_bin"
cp "$source_root/tools/bazel" "$repo_root/tools/bazel"
cp \
    "$source_root/bazel/wrapper_hook/bazel_commands.commands" \
    "$repo_root/bazel/wrapper_hook/bazel_commands.commands"

cat >"$fake_bin/python3" <<'EOF'
#!/usr/bin/env bash
if [[ "${1:-}" == "-c" ]]; then
  exit "${FAKE_PYTHON_VERSION_OK:-1}"
fi

case "${1:-}" in
  */hermetic_container_integration.py)
    if [[ -n "${FAKE_INTEGRATION_MARKER:-}" ]]; then
      printf '%s\n' "${@:3}" >>"$FAKE_INTEGRATION_MARKER"
    fi
    shift
    exec "$@"
    ;;
  */wrapper_hook.py)
    if [[ -n "${FAKE_WRAPPER_PYTHON_MARKER:-}" ]]; then
      printf '%s\n' "${PYTHON3:-}" >"$FAKE_WRAPPER_PYTHON_MARKER"
    fi
    printf '%s\n' "${@:3}" >"$MONGO_BAZEL_WRAPPER_ARGS"
    ;;
  */query_failure_diagnostic.py)
    query_stderr_file="$(mktemp)"
    shift
    "$@" 2>"$query_stderr_file"
    query_exit=$?
    cat "$query_stderr_file" >&2
    rm -f "$query_stderr_file"
    exit "$query_exit"
    ;;
  */post_bazel_hook.py)
    if [[ -n "${FAKE_POSTHOOK_MARKER:-}" ]]; then
      touch "$FAKE_POSTHOOK_MARKER"
    fi
    exit "${FAKE_POSTHOOK_EXIT:-0}"
    ;;
  *)
    shift
    exec "$@"
    ;;
esac
EOF
chmod +x "$fake_bin/python3"
cp "$fake_bin/python3" "$fake_bin/python"

cat >"$fake_bin/bazel-real" <<'EOF'
#!/usr/bin/env bash
if [[ -n "${FAKE_PYHOST_BUILD_MARKER:-}" && "$*" == *"@py_host//:all"* ]]; then
  touch "$FAKE_PYHOST_BUILD_MARKER"
fi

if [[ -n "${FAKE_PYHOST:-}" && "$*" == *"@py_host//:all"* ]]; then
  mkdir -p "$FAKE_BAZEL_EXECROOT" "$(dirname "$FAKE_PYHOST")"
  cp "$FAKE_PYTHON_BINARY" "$FAKE_PYHOST"
  chmod +x "$FAKE_PYHOST"
  ln -s "$FAKE_BAZEL_EXECROOT" "$FAKE_BAZEL_SYMLINK"
  exit 0
fi

if [[ "${1:-}" == "query" ]]; then
  printf '\rQUERY_PROGRESS ' >&2
  printf 'QUERY_RESULT\n'
  exit 0
fi

printf '%s\n' "$*" >"$FAKE_BAZEL_MARKER"
exit "${FAKE_BAZEL_EXIT:-0}"
EOF
chmod +x "$fake_bin/bazel-real"

marker="$test_root/bazel-called"
native_build_args="build --strategy=MongoInstallRule=local //:format"
env \
    BAZELISK_SKIP_WRAPPER=1 \
    BAZEL_REAL="$fake_bin/bazel-real" \
    FAKE_BAZEL_MARKER="$marker" \
    FAKE_PYTHON_VERSION_OK=1 \
    PATH="$fake_bin:$PATH" \
    "$repo_root/tools/bazel" version
grep -qx "version" "$marker"

# Query progress is written to stderr before query results are written to stdout.
# Keep that ordering intact so a carriage-return progress update cannot erase the
# result when both streams are connected to the terminal. The old diagnostic
# wrapper captured stderr and replayed it after stdout, which broke this ordering.
query_tty_output="$test_root/query-tty-output"
query_command_args=(
    env
    BAZELISK_SKIP_WRAPPER=1
    BAZEL_REAL="$fake_bin/bazel-real"
    FAKE_BAZEL_MARKER="$marker"
    FAKE_PYTHON_VERSION_OK=0
    MONGO_BAZEL_USE_HERMETIC_CONTAINER=0
    PATH="$fake_bin:$PATH"
    "$repo_root/tools/bazel"
    query
    //:target
)
if [[ "$OSTYPE" == darwin* ]]; then
    # BSD script takes the transcript file and command as positional arguments.
    script -q "$query_tty_output" "${query_command_args[@]}" >/dev/null
else
    printf -v query_command '%q ' "${query_command_args[@]}"
    script -qefc "$query_command" "$query_tty_output" >/dev/null
fi
query_progress_offset="$(grep -abo 'QUERY_PROGRESS' "$query_tty_output" | cut -d: -f1)"
query_result_offset="$(grep -abo 'QUERY_RESULT' "$query_tty_output" | cut -d: -f1)"
[[ "$query_progress_offset" -lt "$query_result_offset" ]]

integration_marker="$test_root/integration-called"
posthook_marker="$test_root/posthook-called"
rm -f "$marker"
env \
    BAZELISK_SKIP_WRAPPER=1 \
    BAZEL_REAL="$fake_bin/bazel-real" \
    FAKE_BAZEL_MARKER="$marker" \
    FAKE_INTEGRATION_MARKER="$integration_marker" \
    FAKE_POSTHOOK_MARKER="$posthook_marker" \
    FAKE_PYTHON_VERSION_OK=0 \
    PATH="$fake_bin:$PATH" \
    "$repo_root/tools/bazel" clean
grep -qx "clean" "$integration_marker"
grep -qx "clean" "$marker"
[[ ! -e "$posthook_marker" ]]

rm -f "$marker" "$integration_marker"
env \
    BAZELISK_SKIP_WRAPPER=1 \
    BAZEL_REAL="$fake_bin/bazel-real" \
    FAKE_BAZEL_MARKER="$marker" \
    FAKE_INTEGRATION_MARKER="$integration_marker" \
    FAKE_PYTHON_VERSION_OK=0 \
    PATH="$fake_bin:$PATH" \
    "$repo_root/tools/bazel" shutdown
grep -qx "shutdown" "$integration_marker"
grep -qx "shutdown" "$marker"

rm -f "$marker"
env \
    BAZELISK_SKIP_WRAPPER=1 \
    BAZEL_REAL="$fake_bin/bazel-real" \
    FAKE_BAZEL_MARKER="$marker" \
    FAKE_PYTHON_VERSION_OK=1 \
    PATH="$fake_bin:$PATH" \
    "$repo_root/tools/bazel" --version
grep -qx -- "--version" "$marker"

pyhost="$test_root/bazel-output/external/_main~setup_mongo_python_toolchains~py_host/dist/bin/python3"
wrapper_python_marker="$test_root/wrapper-python"
env \
    BAZELISK_SKIP_WRAPPER=1 \
    BAZEL_REAL="$fake_bin/bazel-real" \
    FAKE_BAZEL_EXECROOT="$test_root/bazel-output/execroot/_main" \
    FAKE_BAZEL_MARKER="$marker" \
    FAKE_BAZEL_SYMLINK="$repo_root/bazel-repo" \
    FAKE_PYHOST="$pyhost" \
    FAKE_PYTHON_BINARY="$fake_bin/python3" \
    FAKE_PYTHON_VERSION_OK=0 \
    FAKE_WRAPPER_PYTHON_MARKER="$wrapper_python_marker" \
    MONGO_BAZEL_USE_HERMETIC_CONTAINER=0 \
    PATH="$fake_bin:$PATH" \
    "$repo_root/tools/bazel" build //:format
wrapper_python="$(<"$wrapper_python_marker")"
[[ "$(realpath "$wrapper_python")" == "$(realpath "$pyhost")" ]]
grep -qx "$native_build_args" "$marker"

env \
    BAZELISK_SKIP_WRAPPER=1 \
    BAZEL_REAL="$fake_bin/bazel-real" \
    FAKE_BAZEL_MARKER="$marker" \
    FAKE_POSTHOOK_EXIT=17 \
    FAKE_PYTHON_VERSION_OK=0 \
    MONGO_BAZEL_USE_HERMETIC_CONTAINER=0 \
    PATH="$fake_bin:$PATH" \
    "$repo_root/tools/bazel" build //:format

bazel_exit=0
env \
    BAZELISK_SKIP_WRAPPER=1 \
    BAZEL_REAL="$fake_bin/bazel-real" \
    FAKE_BAZEL_MARKER="$marker" \
    FAKE_BAZEL_EXIT=23 \
    FAKE_PYTHON_VERSION_OK=0 \
    MONGO_BAZEL_USE_HERMETIC_CONTAINER=0 \
    PATH="$fake_bin:$PATH" \
    "$repo_root/tools/bazel" build //:format || bazel_exit=$?
[[ "$bazel_exit" -eq 23 ]]

rm -f "$marker"
for simulated_ostype in darwin23 msys; do
    integration_marker="$test_root/${simulated_ostype}-integration-called"
    env \
        OSTYPE="$simulated_ostype" \
        BAZELISK_SKIP_WRAPPER=1 \
        BAZEL_REAL="$fake_bin/bazel-real" \
        FAKE_BAZEL_MARKER="$marker" \
        FAKE_INTEGRATION_MARKER="$integration_marker" \
        FAKE_PYTHON_VERSION_OK=0 \
        MONGO_BAZEL_USE_HERMETIC_CONTAINER=1 \
        PATH="$fake_bin:$PATH" \
        "$repo_root/tools/bazel" build //:format
    grep -qx "$native_build_args" "$marker"
    [[ ! -e "$integration_marker" ]]
done

mkdir -p "$repo_root/python3-venv/bin"
cp "$fake_bin/python3" "$repo_root/python3-venv/bin/python"
env \
    BAZELISK_SKIP_WRAPPER=1 \
    BAZEL_REAL="$fake_bin/bazel-real" \
    FAKE_BAZEL_MARKER="$marker" \
    FAKE_PYTHON_VERSION_OK=0 \
    PATH="$fake_bin:$PATH" \
    "$repo_root/tools/bazel" version
grep -qx "version" "$marker"

rm "$repo_root/bazel-repo"
rm -f "$marker" "$wrapper_python_marker"
pyhost_build_marker="$test_root/pyhost-build"
env \
    BAZELISK_SKIP_WRAPPER=1 \
    BAZEL_REAL="$fake_bin/bazel-real" \
    FAKE_BAZEL_MARKER="$marker" \
    FAKE_PYHOST_BUILD_MARKER="$pyhost_build_marker" \
    FAKE_PYTHON_VERSION_OK=0 \
    FAKE_WRAPPER_PYTHON_MARKER="$wrapper_python_marker" \
    MONGO_BAZEL_USE_HERMETIC_CONTAINER=0 \
    PATH="$fake_bin:$PATH" \
    "$repo_root/tools/bazel" build //:format
if [[ -e "$pyhost_build_marker" ]]; then
    echo "wrapper bootstrapped @py_host despite having a compatible local Python" >&2
    exit 1
fi
wrapper_python="$(<"$wrapper_python_marker")"
[[ "$(realpath "$wrapper_python")" == "$(realpath "$repo_root/python3-venv/bin/python")" ]]
grep -qx "$native_build_args" "$marker"

rm -f "$marker"
env \
    BAZELISK_SKIP_WRAPPER=1 \
    BAZEL_REAL="$fake_bin/bazel-real" \
    FAKE_BAZEL_MARKER="$marker" \
    FAKE_PYTHON_VERSION_OK=1 \
    MONGO_BAZEL_USE_HERMETIC_CONTAINER=0 \
    PATH="$fake_bin:$PATH" \
    "$repo_root/tools/bazel" version
grep -qx "version" "$marker"
