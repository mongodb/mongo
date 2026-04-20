# Common helpers for CI Bazel scripts (build/run/test)

set -o errexit
set -o pipefail

# Cache the current workspace's output_base in-process so retry and timeout handlers
# can inspect Bazel state without re-entering the server.
BAZEL_EVERGREEN_OUTPUT_BASE="${BAZEL_EVERGREEN_OUTPUT_BASE:-}"

# --- Pre-flight (assumes prelude.sh already sourced by caller) -------------

bazel_evergreen_shutils::activate_and_cd_src() {
    cd src
    set -o verbose
    activate_venv
}

# --- Distro quirks -----------------------------------------------------

bazel_evergreen_shutils::export_ssl_paths_if_needed() {
    if [[ -f /etc/os-release ]]; then
        local DISTRO
        DISTRO=$(awk -F '[="]*' '/^PRETTY_NAME/ { print $2 }' </etc/os-release)
        if [[ "$DISTRO" == "Amazon Linux 2" ]]; then
            export SSL_CERT_DIR=/etc/pki/tls/certs
            export SSL_CERT_FILE=/etc/pki/tls/certs/ca-bundle.crt
        elif [[ "$DISTRO" == "Red Hat Enterprise Linux"* ]]; then
            export SSL_CERT_DIR=/etc/pki/ca-trust/extracted/pem
            export SSL_CERT_FILE=/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem
        fi
    fi
}

bazel_evergreen_shutils::is_macos() {
    local -r os="$(uname -s | tr '[:upper:]' '[:lower:]')"
    [[ "${os}" == "darwin" ]] && return 0 || return 1
}

bazel_evergreen_shutils::is_ppc64le() {
    local -r arch="$(uname -m)"
    [[ "${arch}" == "ppc64le" || "${arch}" == "ppc64" || "${arch}" == "ppc" ]] && return 0 || return 1
}

bazel_evergreen_shutils::is_s390x() {
    local -r arch="$(uname -m)"
    [[ "${arch}" == "s390x" || "${arch}" == "s390" ]] && return 0 || return 1
}

bazel_evergreen_shutils::is_s390x_or_ppc64le() {
    if bazel_evergreen_shutils::is_ppc64le || bazel_evergreen_shutils::is_s390x; then
        return 0
    else
        return 1
    fi
}

bazel_evergreen_shutils::bazel_get_binary_path() {
    if bazel_evergreen_shutils::is_macos; then
        echo "bazel"
    elif bazel_evergreen_shutils::is_s390x_or_ppc64le ||
        grep -q "ID=debian" /etc/os-release ||
        grep -q 'ID="sles"' /etc/os-release; then
        echo "bazel/bazelisk.py"
    else
        echo "bazel"
    fi
}

# --- RBE/local flags + arch tuning ----------------------------------------

bazel_evergreen_shutils::bazel_rbe_supported() {

    local OS ARCH
    OS="$(uname)"
    ARCH="$(uname -m)"

    if [ "$ARCH" == "aarch64" ] || [ "$ARCH" == "arm64" ] || [ "$ARCH" == "x86_64" ]; then
        return 0
    else
        return 1
    fi
}

# Requires: evergreen_remote_exec, task_name (for tests), bazel_args vars optionally.
bazel_evergreen_shutils::compute_local_arg() {
    local mode="${1:-build}" # build|test|run
    local local_arg=""
    if [[ "${evergreen_remote_exec:-}" != "on" ]]; then
        local_arg+=" --jobs=auto"
    elif [[ "$mode" == "test" && "${task_name:-}" == "unit_tests" ]]; then
        local_arg+=" --config=remote_test"
        local_arg+=" --test_timeout=660" # Allow extra 60s for coredump on abort

        # Don't cache test results for merge queue and waterfall tasks initially
        if [[ "${is_commit_queue:-}" != "true" && "${requester:-}" != "commit" ]]; then
            local_arg+=" --cache_test_results=auto"
        fi
    fi

    if bazel_evergreen_shutils::is_ppc64le; then
        local_arg+=" --jobs=48"
    fi
    if bazel_evergreen_shutils::is_s390x; then
        local_arg+=" --jobs=16"
    fi

    # For run-mode, if RBE isn't supported or is disabled explicitly, force local config.
    if [[ "$mode" == "run" ]]; then
        if ! bazel_evergreen_shutils::bazel_rbe_supported || [[ "${evergreen_remote_exec:-}" != "on" ]]; then
            # Keep compatibility with existing pattern:
            local_arg+=" --config=local"
        fi
    fi

    echo "$local_arg"
}

# Keeps only --config flags from a flag string (used to persist consistency)
bazel_evergreen_shutils::extract_config_flags() {
    local all="$*"
    awk '{
    for (i=1;i<=NF;i++) if ($i ~ /^--config(=|$)/) printf "%s ", $i
  }' <<<"$all"
}

# Adds --config=public-release-rbe or --config=public-release-local if this is a release-ish build.
bazel_evergreen_shutils::maybe_release_flag() {
    local local_arg="$1"
    if [[ "${release_rbe:-}" == "true" ]]; then
        echo "$local_arg --config=public-release-rbe" # release with RBE (Remote Build Execution)
    elif [[ "${is_patch:-}" == "true" || -z "${push_bucket:-}" || "${compiling_for_test:-}" == "true" ]]; then
        echo "$local_arg" # non-release
    else
        echo "$local_arg --config=public-release-local" # release without RBE (Remote Build Execution)
    fi
}

# --- Timeouts --------------------------------------------------------------

# Timeout helper: returns a "timeout <secs>" prefix or empty string.
# Prints a one-time warning to stderr if a timeout was requested but no
# timeout binary is available. Supports macOS 'gtimeout' if installed.
# SIGQUIT triggers Bazel/Bazelisk diagnostics but does not reliably stop the
# full process tree, so follow it with a short kill-after window.
bazel_evergreen_shutils::timeout_prefix() {
    local fallback_remote="${1:-}" # "on" = use 3600s default for remote builds
    local need_timeout=""          # "explicit" | "fallback" | ""
    local timeout_bin=""
    local timeout_kill_after_seconds="${BAZEL_EVG_TIMEOUT_KILL_AFTER_SECONDS:-15}"

    # Do we want a timeout?
    if [[ -n "${build_timeout_seconds:-}" ]]; then
        need_timeout="explicit"
    elif [[ "$fallback_remote" == "on" ]]; then
        need_timeout="fallback"
    fi

    # Find a timeout binary (GNU coreutils 'timeout' or macOS 'gtimeout')
    if command -v timeout >/dev/null 2>&1; then
        timeout_bin="timeout"
    elif command -v gtimeout >/dev/null 2>&1; then
        timeout_bin="gtimeout"
    fi

    # If needed but unavailable, warn once and return empty
    if [[ -n "$need_timeout" && -z "$timeout_bin" ]]; then
        if [[ -z "${_BAZEL_EVG_TIMEOUT_WARNED:-}" ]]; then
            if [[ "$need_timeout" == "explicit" ]]; then
                echo "[warn] 'timeout' not found; requested ${build_timeout_seconds}s timeout will be ignored." >&2
            else
                echo "[warn] 'timeout' not found; remote-build fallback timeout (3600s) will be ignored." >&2
            fi
            # Helpful hint for macOS users
            if bazel_evergreen_shutils::is_macos; then
                echo "[hint] On macOS, install coreutils: 'brew install coreutils' (provides 'gtimeout')." >&2
            fi
            _BAZEL_EVG_TIMEOUT_WARNED=1
        fi
        echo "" # no timeout prefix
        return 0
    fi

    # Produce the prefix if we have a binary
    if [[ -n "$timeout_bin" ]]; then
        if [[ "$need_timeout" == "explicit" ]]; then
            echo "$timeout_bin -s QUIT -k ${timeout_kill_after_seconds}s ${build_timeout_seconds}"
        elif [[ "$need_timeout" == "fallback" ]]; then
            echo "$timeout_bin -s QUIT -k ${timeout_kill_after_seconds}s 3600"
        else
            echo ""
        fi
    else
        echo ""
    fi
}

bazel_evergreen_shutils::is_timeout_exit_code() {
    local ret="$1"
    local timeout_str="${2:-}"
    local timeout_duration="${3:-}"
    local attempt_elapsed_seconds="${4:-0}"

    if [[ "$ret" -eq 124 ]]; then
        return 0
    fi

    # GNU timeout returns 137 when -k escalates from SIGQUIT to SIGKILL.
    if [[ -n "$timeout_str" &&
        "$timeout_str" == *" -k "* &&
        -n "$timeout_duration" &&
        "$ret" -eq 137 &&
        "$attempt_elapsed_seconds" -ge "$timeout_duration" ]]; then
        return 0
    fi

    return 1
}

# --- Bazel server lifecycle & OOM-detect retry -----------------------------

bazel_evergreen_shutils::cache_bazel_output_base() {
    local BAZEL_BINARY="$1"
    local output_base=""

    if [[ -n "${BAZEL_EVERGREEN_OUTPUT_BASE:-}" ]]; then
        return 0
    fi

    # `bazel --batch info output_base` kills a live server because the startup
    # options differ. Cache the normal output_base once while the server is healthy.
    output_base="$("$BAZEL_BINARY" info output_base 2>/dev/null)" || return 1
    [[ -n "$output_base" ]] || return 1
    BAZEL_EVERGREEN_OUTPUT_BASE="$output_base"
}

bazel_evergreen_shutils::bazel_output_base() {
    local BAZEL_BINARY="$1"
    bazel_evergreen_shutils::cache_bazel_output_base "$BAZEL_BINARY" || return 1
    echo "$BAZEL_EVERGREEN_OUTPUT_BASE"
}

bazel_evergreen_shutils::bazel_pidfile_path() {
    local BAZEL_BINARY="$1"
    local ob
    ob="$(bazel_evergreen_shutils::bazel_output_base "$BAZEL_BINARY")" || return 1
    echo "${ob}/server/server.pid.txt"
}

bazel_evergreen_shutils::bazel_server_pid() {
    local BAZEL_BINARY="$1"
    local pf pid
    pf="$(bazel_evergreen_shutils::bazel_pidfile_path "$BAZEL_BINARY")" || return 1
    [[ -f "$pf" ]] || return 1
    pid="$(cat "$pf" 2>/dev/null || true)"
    [[ "$pid" =~ ^[0-9]+$ ]] || return 1
    echo "$pid"
}

bazel_evergreen_shutils::is_bazel_server_running() {
    local BAZEL_BINARY="$1"
    local pid
    pid="$(bazel_evergreen_shutils::bazel_server_pid "$BAZEL_BINARY" 2>/dev/null || true)"
    [[ -n "$pid" ]] || return 1
    if kill -0 "$pid" 2>/dev/null; then
        return 0
    else
        return 1
    fi
}

bazel_evergreen_shutils::print_bazel_server_pid() {
    local BAZEL_BINARY="$1"
    local pf pid
    pf="$(bazel_evergreen_shutils::bazel_pidfile_path "$BAZEL_BINARY")" || {
        echo "Bazel server pidfile not found (output_base: $(bazel_evergreen_shutils::bazel_output_base "$BAZEL_BINARY" 2>/dev/null || true))"
        return 0
    }
    if [[ -f "$pf" ]]; then
        pid="$(cat "$pf" 2>/dev/null || true)"
        echo "Bazel server pidfile: $pf (PID=${pid:-unknown})"
    else
        echo "Bazel server pidfile not found yet (output_base: $(bazel_evergreen_shutils::bazel_output_base "$BAZEL_BINARY" 2>/dev/null || true))"
    fi
}

bazel_evergreen_shutils::fast_bazel_server_pids() {
    local pid
    local -a live_pids=()
    local -A seen_pids=()

    while IFS= read -r pid; do
        if [[ ! "$pid" =~ ^[0-9]+$ ]] || [[ -n "${seen_pids[$pid]:-}" ]]; then
            continue
        fi
        seen_pids["$pid"]=1
        if kill -0 "$pid" 2>/dev/null; then
            live_pids+=("$pid")
        fi
    done < <(pgrep -f "java.*bazel" 2>/dev/null || true)

    if [[ ${#live_pids[@]} -eq 0 ]]; then
        return 1
    fi

    printf '%s\n' "${live_pids[@]}"
}

bazel_evergreen_shutils::bazel_server_pids() {
    local BAZEL_BINARY="$1"
    local pf pid
    local -a candidate_pids=()
    local -a live_pids=()
    local -A seen_pids=()

    pf="$(bazel_evergreen_shutils::bazel_pidfile_path "$BAZEL_BINARY" 2>/dev/null)" || true
    if [[ -f "$pf" ]]; then
        pid="$(cat "$pf" 2>/dev/null || true)"
        if [[ "$pid" =~ ^[0-9]+$ ]]; then
            candidate_pids+=("$pid")
        fi
    fi

    while IFS= read -r pid; do
        if [[ "$pid" =~ ^[0-9]+$ ]]; then
            candidate_pids+=("$pid")
        fi
    done < <(pgrep -f "java.*bazel" 2>/dev/null || true)

    for pid in "${candidate_pids[@]}"; do
        if [[ -n "${seen_pids[$pid]:-}" ]]; then
            continue
        fi
        seen_pids["$pid"]=1
        if kill -0 "$pid" 2>/dev/null; then
            live_pids+=("$pid")
        fi
    done

    if [[ ${#live_pids[@]} -eq 0 ]]; then
        return 1
    fi

    printf '%s\n' "${live_pids[@]}"
}

bazel_evergreen_shutils::bazel_cache_pidfiles() {
    local pattern pidfile
    local -a patterns=(
        "${HOME}/.cache/bazel/_bazel_*/*/server/server.pid.txt"
        "/private/var/tmp/_bazel_*/*/server/server.pid.txt"
        "/var/tmp/_bazel_*/*/server/server.pid.txt"
    )

    shopt -s nullglob
    for pattern in "${patterns[@]}"; do
        for pidfile in $pattern; do
            [[ -f "$pidfile" ]] && echo "$pidfile"
        done
    done
    shopt -u nullglob
}

bazel_evergreen_shutils::bazel_pidfile_path_for_pid() {
    local server_pid="$1"
    local pidfile candidate_pid

    [[ "$server_pid" =~ ^[0-9]+$ ]] || return 1

    while IFS= read -r pidfile; do
        candidate_pid="$(cat "$pidfile" 2>/dev/null || true)"
        if [[ "$candidate_pid" == "$server_pid" ]]; then
            echo "$pidfile"
            return 0
        fi
    done < <(bazel_evergreen_shutils::bazel_cache_pidfiles)

    return 1
}

bazel_evergreen_shutils::request_bazel_jvm_dump() {
    local BAZEL_BINARY="$1"
    local pid
    local signaled_pid=0

    echo "Scanning for bazel server processes to signal." >&2

    while IFS= read -r pid; do
        [[ -z "$pid" ]] && continue
        echo "Sending SIGQUIT to bazel process ${pid}" >&2
        if kill -QUIT "$pid" 2>/dev/null; then
            signaled_pid=1
        else
            echo "Failed to send SIGQUIT to bazel process ${pid}" >&2
        fi
    done < <(bazel_evergreen_shutils::fast_bazel_server_pids || true)

    if [[ "$signaled_pid" -eq 0 ]]; then
        echo "No bazel process found to signal." >&2
        return 1
    fi

    # Bazel's JVM writes thread dumps asynchronously after SIGQUIT.
    sleep 5
}

bazel_evergreen_shutils::terminate_bazel_servers() {
    local pid
    local -a bazel_server_pids=()
    local -a stubborn_pids=()

    while IFS= read -r pid; do
        [[ "$pid" =~ ^[0-9]+$ ]] || continue
        bazel_server_pids+=("$pid")
    done < <(bazel_evergreen_shutils::fast_bazel_server_pids || true)

    if [[ ${#bazel_server_pids[@]} -eq 0 ]]; then
        echo "No bazel server processes found to terminate." >&2
        return 0
    fi

    echo "Stopping bazel server processes: ${bazel_server_pids[*]}" >&2
    kill -TERM "${bazel_server_pids[@]}" 2>/dev/null || true
    sleep 5

    for pid in "${bazel_server_pids[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            stubborn_pids+=("$pid")
        fi
    done

    if [[ ${#stubborn_pids[@]} -eq 0 ]]; then
        return 0
    fi

    echo "Force killing bazel server processes: ${stubborn_pids[*]}" >&2
    kill -KILL "${stubborn_pids[@]}" 2>/dev/null || true
    sleep 1
}

bazel_evergreen_shutils::bazel_jvm_out_snapshot_dir() {
    echo "bazel_jvm_outs"
}

bazel_evergreen_shutils::bazel_jvm_out_path_for_pid() {
    local server_pid="$1"
    local pidfile output_base candidate

    pidfile="$(bazel_evergreen_shutils::bazel_pidfile_path_for_pid "$server_pid")" || return 1
    output_base="$(dirname "$(dirname "$pidfile")")"

    for candidate in "${output_base}/server/jvm.out" "${output_base}/jvm.out"; do
        if [[ -f "$candidate" ]]; then
            echo "$candidate"
            return 0
        fi
    done

    echo "No bazel jvm.out file found for pid ${server_pid} under ${output_base}" >&2
    return 1
}

bazel_evergreen_shutils::bazel_jvm_out_path() {
    local BAZEL_BINARY="$1"
    local output_base jvm_out_path=""
    local candidate

    output_base="$(bazel_evergreen_shutils::bazel_output_base "$BAZEL_BINARY")" || {
        echo "Unable to determine bazel output_base" >&2
        return 1
    }

    for candidate in "${output_base}/server/jvm.out" "${output_base}/jvm.out"; do
        if [[ -f "$candidate" ]]; then
            jvm_out_path="$candidate"
            break
        fi
    done

    if [[ -z "$jvm_out_path" ]]; then
        echo "No bazel jvm.out file found under ${output_base}" >&2
        return 1
    fi

    echo "$jvm_out_path"
}

bazel_evergreen_shutils::capture_bazel_jvm_out() {
    local BAZEL_BINARY="$1"
    local server_pid="${2:-}"
    local jvm_out_path snapshot_dir timestamp output_prefix output_file
    local capture_index=1

    if [[ -z "$server_pid" ]]; then
        server_pid="$(bazel_evergreen_shutils::bazel_server_pid "$BAZEL_BINARY" 2>/dev/null || true)"
    fi
    if [[ -z "$server_pid" ]]; then
        while IFS= read -r server_pid; do
            [[ -n "$server_pid" ]] && break
        done < <(bazel_evergreen_shutils::fast_bazel_server_pids || true)
    fi

    if [[ -n "$server_pid" ]]; then
        jvm_out_path="$(bazel_evergreen_shutils::bazel_jvm_out_path_for_pid "$server_pid" 2>/dev/null || true)"
    fi
    if [[ -z "$jvm_out_path" ]]; then
        jvm_out_path="$(bazel_evergreen_shutils::bazel_jvm_out_path "$BAZEL_BINARY" 2>/dev/null || true)"
    fi
    [[ -n "$jvm_out_path" ]] || return 1

    snapshot_dir="$(bazel_evergreen_shutils::bazel_jvm_out_snapshot_dir)"
    mkdir -p "$snapshot_dir"

    timestamp=$(date +%Y%m%d_%H%M%S)
    if [[ -n "$server_pid" ]]; then
        output_prefix="${snapshot_dir}/bazel_jvm_out_pid${server_pid}_${timestamp}"
    else
        output_prefix="${snapshot_dir}/bazel_jvm_out_pidunknown_${timestamp}"
    fi

    output_file="${output_prefix}.txt"
    while [[ -e "$output_file" ]]; do
        output_file="${output_prefix}_${capture_index}.txt"
        ((capture_index++))
    done

    cp "$jvm_out_path" "$output_file"
    echo "Captured bazel jvm.out from ${jvm_out_path} to $(pwd)/${output_file}" >&2
    echo "$output_file"
}

bazel_evergreen_shutils::package_bazel_jvm_out() {
    local BAZEL_BINARY="$1"
    local archive_path="${2:-jvm.out.tar.gz}"
    local snapshot_dir live_server_pid=""
    local -a snapshots=()

    snapshot_dir="$(bazel_evergreen_shutils::bazel_jvm_out_snapshot_dir)"
    mkdir -p "$snapshot_dir"

    shopt -s nullglob
    snapshots=("${snapshot_dir}"/*)
    shopt -u nullglob

    while IFS= read -r live_server_pid; do
        [[ -n "$live_server_pid" ]] && break
    done < <(bazel_evergreen_shutils::fast_bazel_server_pids || true)

    if [[ -n "$live_server_pid" || ${#snapshots[@]} -eq 0 ]]; then
        bazel_evergreen_shutils::capture_bazel_jvm_out "$BAZEL_BINARY" "$live_server_pid" >/dev/null || {
            if [[ ${#snapshots[@]} -eq 0 ]]; then
                return 1
            fi
        }
        shopt -s nullglob
        snapshots=("${snapshot_dir}"/*)
        shopt -u nullglob
    fi

    if [[ ${#snapshots[@]} -eq 0 ]]; then
        echo "No captured bazel jvm.out files found under $(pwd)/${snapshot_dir}" >&2
        return 1
    fi

    rm -f "$archive_path"
    tar -czf "$archive_path" -C "$(dirname "$snapshot_dir")" "$(basename "$snapshot_dir")"
    echo "Archived ${#snapshots[@]} bazel jvm dump file(s) from $(pwd)/${snapshot_dir} to $(pwd)/${archive_path}" >&2
}

bazel_evergreen_shutils::jstack_bazel() {
    # Find all bazel processes (Java processes with "bazel" in command line)
    local pids
    pids=$(bazel_evergreen_shutils::fast_bazel_server_pids || true)
    if [[ -z "$pids" ]]; then
        return 1
    fi

    # Skip if jstack is not available
    if ! command -v jstack >/dev/null 2>&1; then
        return 1
    fi

    local timestamp
    timestamp=$(date +%Y%m%d_%H%M%S)

    for pid in $pids; do
        local output_file="bazel_jstack_${timestamp}_pid${pid}.txt"
        jstack "$pid" >"$output_file" 2>&1
    done
}

# Starts server (if needed) and prints PID. Safe to call multiple times.
bazel_evergreen_shutils::ensure_server_and_print_pid() {
    local BAZEL_BINARY="$1"
    _IGN=$("$BAZEL_BINARY" info >/dev/null 2>&1 || true)
    bazel_evergreen_shutils::cache_bazel_output_base "$BAZEL_BINARY" || true
    bazel_evergreen_shutils::print_bazel_server_pid "$BAZEL_BINARY"
}

bazel_evergreen_shutils::write_last_engflow_link() {
    engflow_link=$(grep -Eo 'https://[a-zA-Z0-9./?_=-]+' ${last_command_log_path} | grep 'sodalite\.cluster\.engflow\.com' | tail -n 1)
    echo ${engflow_link} >.engflow_link
}

# Generic retry wrapper:
#   $1: attempts
#   $3: bazel binary
#   $4..: full bazel subcommand + args (e.g. "build --verbose_failures ...")
# Special handling:
#   - exit 124/137 -> timeout
#   - server death (pid missing) -> restart, then retry
# Returns with global RET set.
bazel_evergreen_shutils::retry_bazel_cmd() {
    local attempts="$1"
    shift
    local BAZEL_BINARY="$1"
    shift

    local timeout_str="$(bazel_evergreen_shutils::timeout_prefix "${evergreen_remote_exec:-}")"
    local timeout_duration=""
    if [[ -n "$timeout_str" ]]; then
        timeout_duration=$(echo "$timeout_str" | awk '{print $NF}')
    fi

    # Cache output_base before the main Bazel command runs so later retry and
    # timeout handling can read pidfiles and JVM logs without invoking Bazel again.
    bazel_evergreen_shutils::cache_bazel_output_base "$BAZEL_BINARY" || true

    # Get command log path for usage afterwards
    # Use the selected Bazel binary so PPC/s390x don't fall back to a different
    # bazel on PATH with different JDK behavior.
    last_command_log_path=$("$BAZEL_BINARY" info command_log 2>/dev/null || true)

    # Everything else is the Bazel subcommand + flags (and possibly redirections/pipes).
    # We *intentionally* keep it as raw words and reassemble to a single string for eval.
    local raw_rest=("$@")

    # Once we detect an OOM/server-death, we enable the guard for subsequent attempts.
    local use_oom_guard=false
    local -r OOM_GUARD_FLAG='--local_resources=cpu=HOST_CPUS*.5'

    # Helper: does the current command string already include a local_resources flag?
    _cmd_has_local_resources() {
        [[ "$1" == *"--local_cpu_resources"* ]] || [[ "$1" == *"--local_ram_resources"* ]] || [[ "$1" == *"--local_resources"* ]]
    }

    local RET=1

    for i in $(seq 1 "$attempts"); do
        echo "Attempt ${i}/${attempts}…" >&2

        # Ensure/refresh server & pid before we run (helps produce a fresh pidfile too).
        if ! bazel_evergreen_shutils::is_bazel_server_running "$BAZEL_BINARY"; then
            echo "[retry ${i}] Bazel server not running (likely OOM/killed); restarting…" >&2
            "$BAZEL_BINARY" info >/dev/null 2>&1 || true
            bazel_evergreen_shutils::print_bazel_server_pid "$BAZEL_BINARY" >&2
        fi

        local attempt_bazel_server_pid=""
        attempt_bazel_server_pid="$(bazel_evergreen_shutils::bazel_server_pid "$BAZEL_BINARY" 2>/dev/null || true)"

        # Reassemble the caller’s words into a single command string for eval.
        # We deliberately do *not* try to be clever here—this restores legacy behavior
        # where quoted pieces inside variables (e.g., --base_dir="..") are honored by the shell.
        local cmd="\"$BAZEL_BINARY\""
        local w
        for w in "${raw_rest[@]}"; do
            cmd+=" $w"
        done

        # If OOM guard is enabled and not already present, append it.
        # (Safe with eval; if the caller added redirections earlier, this is still just an arg.)
        if $use_oom_guard && ! _cmd_has_local_resources "$cmd"; then
            echo "[retry ${i}] Applying OOM guard: ${OOM_GUARD_FLAG}" >&2
            cmd+=" ${OOM_GUARD_FLAG}"
        fi

        local jstack_dumper_pid=""

        # Prefix timeout, if any.
        if [[ -n "$timeout_str" ]]; then
            cmd="${timeout_str} ${cmd}"

            # Start a background monitor to run jstack 5 seconds before the timeout will expire.
            # This is useful information for debugging a rare hang in bazel where the build gets
            # stuck.
            if [[ $timeout_duration -gt 5 ]]; then
                set -m # Enable job control to create a process group
                (
                    sleep $((timeout_duration - 5))
                    bazel_evergreen_shutils::jstack_bazel "$BAZEL_BINARY" || true
                ) &
                jstack_dumper_pid=$!
                set +m # Disable job control
            fi
        fi

        # Run it.
        # NOTE: We *do not* add any redirections here; caller controls logging completely.
        local attempt_start_epoch
        local attempt_elapsed_seconds=0
        attempt_start_epoch=$(date +%s)
        if eval $env "$cmd"; then
            RET=0
            # Kill the jstack dumper if still running
            if [[ -n "$jstack_dumper_pid" ]]; then
                kill -- -$jstack_dumper_pid 2>/dev/null || true
                wait $jstack_dumper_pid 2>/dev/null || true
            fi
            break
        else
            RET=$?
            attempt_elapsed_seconds=$(($(date +%s) - attempt_start_epoch))
            # Kill the jstack dumper if still running
            if [[ -n "$jstack_dumper_pid" ]]; then
                kill -- -$jstack_dumper_pid 2>/dev/null || true
                wait $jstack_dumper_pid 2>/dev/null || true
            fi
        fi

        if bazel_evergreen_shutils::is_timeout_exit_code "$RET" "$timeout_str" "$timeout_duration" "$attempt_elapsed_seconds"; then
            if [[ $RET -eq 137 ]]; then
                echo "Bazel timed out and was force-killed after SIGQUIT." >&2
            else
                echo "Bazel timed out." >&2
                bazel_evergreen_shutils::request_bazel_jvm_dump "$BAZEL_BINARY" || true
            fi
            bazel_evergreen_shutils::capture_bazel_jvm_out "$BAZEL_BINARY" "$attempt_bazel_server_pid" >/dev/null || true
            bazel_evergreen_shutils::terminate_bazel_servers || true
        elif ! bazel_evergreen_shutils::is_bazel_server_running "$BAZEL_BINARY"; then
            echo "[retry ${i}] Bazel server down (OOM/killed). Enabling OOM guard for next attempt and restarting…" >&2
            use_oom_guard=true
            "$BAZEL_BINARY" shutdown || true
            "$BAZEL_BINARY" info >/dev/null 2>&1 || true
            bazel_evergreen_shutils::print_bazel_server_pid "$BAZEL_BINARY" >&2
        else
            if [[ ${RETRY_ON_FAIL:-0} -eq 1 ]]; then
                echo "Bazel failed (exit=$RET); restarting server before retry..." >&2
                "$BAZEL_BINARY" shutdown || true
            else
                break
            fi
        fi

        sleep 60
    done

    return "$RET"

}

# --- Test helpers ----------------------------------------------------------

# Multiplies test timeout on slow arches and appends to bazel_args
bazel_evergreen_shutils::maybe_scale_test_timeout_and_append() {
    if [[ -n "${test_timeout_sec:-}" ]]; then
        local scaled="$test_timeout_sec"
        if bazel_evergreen_shutils::is_s390x_or_ppc64le; then
            scaled=$((test_timeout_sec * 4))
        fi
        bazel_args="${bazel_args:-} --test_timeout=${scaled}"
    fi
}

# Queries all resmoke_config targets and outputs YAML key-value pairs.
# Usage: bazel_evergreen_shutils::query_resmoke_configs <bazel_binary> <flags> <output_file>
#   example: bazel_evergreen_shutils::query_resmoke_configs "$BAZEL_BINARY" "${CONFIG_FLAGS}" "resmoke_suite_configs.yml"
# Outputs YAML entries like:
#   //buildscripts/resmokeconfig:core_config: bazel-out/k8-fastbuild/bin/buildscripts/resmokeconfig/core.yml
bazel_evergreen_shutils::query_resmoke_configs() {
    local BAZEL_BINARY="$1"
    local FLAGS="$2"
    local OUTPUT_FILE="$3"

    ${BAZEL_BINARY} cquery ${FLAGS} 'kind(resmoke_config, //...)' \
        --output=starlark \
        --starlark:expr='": ".join([str(target.label).replace("@@","")] + [f.path for f in target.files.to_list()])' \
        >"${OUTPUT_FILE}"
}
