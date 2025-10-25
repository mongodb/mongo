# Common helpers for CI Bazel scripts (build/run/test)

set -o errexit
set -o pipefail

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
    DISTRO=$(awk -F '[="]*' '/^PRETTY_NAME/ { print $2 }' < /etc/os-release)
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
  elif bazel_evergreen_shutils::is_s390x_or_ppc64le \
    || grep -q "ID=debian" /etc/os-release \
    || grep -q 'ID="sles"' /etc/os-release; then
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
  }' <<< "$all"
}

# Adds --config=public-release if this is a release-ish build.
bazel_evergreen_shutils::maybe_release_flag() {
  if [[ "${is_patch:-}" == "true" || -z "${push_bucket:-}" || "${compiling_for_test:-}" == "true" ]]; then
    echo "" # non-release
  else
    echo "--config=public-release"
  fi
}

# --- Timeouts --------------------------------------------------------------

# Timeout helper: returns a "timeout <secs>" prefix or empty string.
# Prints a one-time warning to stderr if a timeout was requested but no
# timeout binary is available. Supports macOS 'gtimeout' if installed.
bazel_evergreen_shutils::timeout_prefix() {
  local fallback_remote="${1:-}" # "on" = use 3600s default for remote builds
  local need_timeout=""          # "explicit" | "fallback" | ""
  local timeout_bin=""

  # Do we want a timeout?
  if [[ -n "${build_timeout_seconds:-}" ]]; then
    need_timeout="explicit"
  elif [[ "$fallback_remote" == "on" ]]; then
    need_timeout="fallback"
  fi

  # Find a timeout binary (GNU coreutils 'timeout' or macOS 'gtimeout')
  if command -v timeout > /dev/null 2>&1; then
    timeout_bin="timeout"
  elif command -v gtimeout > /dev/null 2>&1; then
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
      echo "$timeout_bin ${build_timeout_seconds}"
    elif [[ "$need_timeout" == "fallback" ]]; then
      echo "$timeout_bin 3600"
    else
      echo ""
    fi
  else
    echo ""
  fi
}

# --- Bazel server lifecycle & OOM-detect retry -----------------------------

bazel_evergreen_shutils::bazel_output_base() {
  local BAZEL_BINARY="$1"
  "$BAZEL_BINARY" info output_base 2> /dev/null
}

bazel_evergreen_shutils::bazel_pidfile_path() {
  local BAZEL_BINARY="$1"
  local ob
  ob="$(bazel_evergreen_shutils::bazel_output_base "$BAZEL_BINARY")" || return 1
  echo "${ob}/server/server.pid.txt"
}

bazel_evergreen_shutils::is_bazel_server_running() {
  local BAZEL_BINARY="$1"
  local pf pid
  pf="$(bazel_evergreen_shutils::bazel_pidfile_path "$BAZEL_BINARY")" || return 1
  [[ -f "$pf" ]] || return 1
  pid="$(cat "$pf" 2> /dev/null || true)"
  [[ -n "$pid" ]] || return 1
  if kill -0 "$pid" 2> /dev/null; then
    return 0
  else
    return 1
  fi
}

bazel_evergreen_shutils::print_bazel_server_pid() {
  local BAZEL_BINARY="$1"
  local pf pid
  pf="$(bazel_evergreen_shutils::bazel_pidfile_path "$BAZEL_BINARY")" || {
    echo "Bazel server pidfile not found (output_base: $(bazel_evergreen_shutils::bazel_output_base "$BAZEL_BINARY" 2> /dev/null || true))"
    return 0
  }
  if [[ -f "$pf" ]]; then
    pid="$(cat "$pf" 2> /dev/null || true)"
    echo "Bazel server pidfile: $pf (PID=${pid:-unknown})"
  else
    echo "Bazel server pidfile not found yet (output_base: $(bazel_evergreen_shutils::bazel_output_base "$BAZEL_BINARY" 2> /dev/null || true))"
  fi
}

# Starts server (if needed) and prints PID. Safe to call multiple times.
bazel_evergreen_shutils::ensure_server_and_print_pid() {
  local BAZEL_BINARY="$1"
  _IGN=$("$BAZEL_BINARY" info > /dev/null 2>&1 || true)
  bazel_evergreen_shutils::print_bazel_server_pid "$BAZEL_BINARY"
}

# Generic retry wrapper:
#   $1: attempts
#   $3: bazel binary
#   $4..: full bazel subcommand + args (e.g. "build --verbose_failures ...")
# Special handling:
#   - exit 124 -> timeout
#   - server death (pid missing) -> restart, then retry
# Returns with global RET set.
bazel_evergreen_shutils::retry_bazel_cmd() {
  local attempts="$1"
  shift
  local BAZEL_BINARY="$1"
  shift

  local timeout_str="$(bazel_evergreen_shutils::timeout_prefix "${evergreen_remote_exec:-}")"

  # Everything else is the Bazel subcommand + flags (and possibly redirections/pipes).
  # We *intentionally* keep it as raw words and reassemble to a single string for eval.
  local raw_rest=("$@")

  # Once we detect an OOM/server-death, we enable the guard for subsequent attempts.
  local use_oom_guard=false
  local -r OOM_GUARD_FLAG='--local_resources=HOST_CPUS*.5'

  # Helper: does the current command string already include a local_resources flag?
  _cmd_has_local_resources() {
    [[ "$1" == *"--local_resources"* ]]
  }

  local RET=1

  for i in $(seq 1 "$attempts"); do
    echo "Attempt ${i}/${attempts}…" >&2

    # Ensure/refresh server & pid before we run (helps produce a fresh pidfile too).
    if ! bazel_evergreen_shutils::is_bazel_server_running "$BAZEL_BINARY"; then
      echo "[retry ${i}] Bazel server not running (likely OOM/killed); restarting…" >&2
      "$BAZEL_BINARY" info > /dev/null 2>&1 || true
      bazel_evergreen_shutils::print_bazel_server_pid "$BAZEL_BINARY" >&2
    fi

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

    # Prefix timeout, if any.
    if [[ -n "$timeout_str" ]]; then
      cmd="${timeout_str} ${cmd}"
    fi

    # Run it.
    # NOTE: We *do not* add any redirections here; caller controls logging completely.
    if eval $env "$cmd"; then
      RET=0
      break
    else
      RET=$?
    fi

    # Classify failure & decide on guard for next attempt.
    [[ $RET -eq 124 ]] && echo "Bazel timed out." >&2

    if ! bazel_evergreen_shutils::is_bazel_server_running "$BAZEL_BINARY"; then
      echo "[retry ${i}] Bazel server down (OOM/killed). Enabling OOM guard for next attempt and restarting…" >&2
      use_oom_guard=true
      "$BAZEL_BINARY" shutdown || true
      "$BAZEL_BINARY" info > /dev/null 2>&1 || true
      bazel_evergreen_shutils::print_bazel_server_pid "$BAZEL_BINARY" >&2
    else
      echo "Bazel failed (exit=$RET); restarting server before retry…" >&2
      "$BAZEL_BINARY" shutdown || true
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
