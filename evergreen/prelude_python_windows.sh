_python_works() {
    local candidate="$1"

    [ -f "$candidate" ] || return 1
    PYTHONDONTWRITEBYTECODE=1 "$candidate" -c 'raise SystemExit(0 if __import__("sys").version_info >= (3, 13) else 1)' >/dev/null 2>&1
}

_bazel_real_command() {
    if [ -n "${BAZEL_REAL:-}" ]; then
        printf '%s\n' "$BAZEL_REAL"
        return 0
    fi

    command -v bazel
}

_bazel_repo_root() {
    if [ -n "${evergreen_dir:-}" ]; then
        cd "$evergreen_dir/.." && pwd
    else
        cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd
    fi
}

_bazel_query_output_base() {
    local bazel_command="$1" repo_root output_base

    repo_root=$(_bazel_repo_root) || return 1
    output_base=$(cd "$repo_root" && BAZELISK_SKIP_WRAPPER=1 MONGO_BAZEL_IN_HERMETIC_CONTAINER=1 "$bazel_command" info output_base 2>/dev/null) || return 1
    if command -v cygpath >/dev/null 2>&1; then
        output_base=$(cygpath -u "$output_base")
    fi
    printf '%s\n' "$output_base"
}

_bazel_output_base_from_symlink() {
    local repo_root symlink execroot

    repo_root=$(_bazel_repo_root) || return 1
    symlink="$repo_root/bazel-$(basename "$repo_root")"
    [ -d "$symlink" ] || return 1

    execroot=$(cd "$symlink" 2>/dev/null && pwd -P) || return 1
    cd "$execroot/../.." && pwd -P
}

_bazel_py_host_python() {
    local bazel_command repo_root output_base candidate

    if ! bazel_command=$(_bazel_real_command); then
        echo "ERROR: Could not find Bazel to bootstrap the hermetic Python toolchain." >&2
        return 1
    fi
    if ! repo_root=$(_bazel_repo_root); then
        echo "ERROR: Could not locate the checkout root to bootstrap the hermetic Python toolchain." >&2
        return 1
    fi

    # BAZEL_REAL may be reported as a native Windows path when this script is
    # sourced from Cygwin. Convert it before executing it from Bash.
    if command -v cygpath >/dev/null 2>&1; then
        bazel_command=$(cygpath -u "$bazel_command")
    fi

    # Skip the Python-dependent wrapper in Bazelisk itself: tools/bazel.bat
    # does not honor the Bash wrapper's hermetic-container bypass. If info
    # fails on a fresh host, query again after materializing the repositories.
    output_base=$(_bazel_query_output_base "$bazel_command" || true)

    candidate="$output_base/external/+setup_mongo_python_toolchains+py_host/dist/python.exe"
    if ! _python_works "$candidate"; then
        echo "Windows Python is unavailable; bootstrapping Bazel @py_host..." >&2
        if ! (cd "$repo_root" && BAZELISK_SKIP_WRAPPER=1 MONGO_BAZEL_IN_HERMETIC_CONTAINER=1 "$bazel_command" build \
            --bes_backend= \
            --bes_results_url= \
            --workspace_status_command= \
            @py_host//:all >&2); then
            echo "Bazel could not bootstrap @py_host; retrying with the local configuration." >&2
            (cd "$repo_root" && BAZELISK_SKIP_WRAPPER=1 MONGO_BAZEL_IN_HERMETIC_CONTAINER=1 "$bazel_command" build \
                --config=local \
                --workspace_status_command= \
                @py_host//:all >&2) || return 1
        fi
    fi

    if ! _python_works "$candidate"; then
        output_base=$(_bazel_query_output_base "$bazel_command" || true)
        if [ -z "$output_base" ]; then
            output_base=$(_bazel_output_base_from_symlink || true)
        fi
        candidate="$output_base/external/+setup_mongo_python_toolchains+py_host/dist/python.exe"
    fi

    if ! _python_works "$candidate"; then
        echo "ERROR: Bazel bootstrapped @py_host, but its Windows Python interpreter was not found at:" >&2
        echo "       $candidate" >&2
        return 1
    fi

    printf '%s\n' "$candidate"
}

_windows_python() {
    local host_python="$1" repo_root runtime_dir candidate

    if _python_works "$host_python"; then
        printf '%s\n' "$host_python"
        return 0
    fi

    repo_root=$(_bazel_repo_root) || return 1
    runtime_dir="$repo_root/.tmp/evergreen-python"
    if [ ! -f "$runtime_dir/.evergreen-complete" ] || ! _python_works "$runtime_dir/python.exe"; then
        candidate=$(_bazel_py_host_python) || return 1
        # Venvs keep a reference to their base runtime. Keep it outside Bazel's
        # output base so clearing or switching that base cannot break the venvs
        # or lock DLLs that Bazel needs to replace during a repository fetch.
        mkdir -p "$runtime_dir" || return 1
        # An executable can pass the version probe even if a prior copy failed
        # before all extension DLLs and library files arrived.
        rm -f "$runtime_dir/.evergreen-complete" || return 1
        cp -R "$(dirname "$candidate")/." "$runtime_dir/" || return 1
        _python_works "$runtime_dir/python.exe" || return 1
        touch "$runtime_dir/.evergreen-complete" || return 1
    fi

    _python_works "$runtime_dir/python.exe" || return 1
    printf '%s\n' "$runtime_dir/python.exe"
}
