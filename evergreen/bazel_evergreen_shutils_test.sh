#!/usr/bin/env bash

set -euo pipefail

source "${TEST_SRCDIR}/${TEST_WORKSPACE}/evergreen/bazel_evergreen_shutils.sh"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

assert_eq() {
    local expected="$1"
    local actual="$2"
    local message="$3"

    if [[ "$expected" != "$actual" ]]; then
        fail "${message}: expected '${expected}', got '${actual}'"
    fi
}

assert_contains() {
    local haystack="$1"
    local needle="$2"
    local message="$3"

    if [[ "$haystack" != *"$needle"* ]]; then
        fail "${message}: expected output to contain '${needle}'"
    fi
}

assert_not_contains() {
    local haystack="$1"
    local needle="$2"
    local message="$3"

    if [[ "$haystack" == *"$needle"* ]]; then
        fail "${message}: expected output not to contain '${needle}'"
    fi
}

# Replaces a function for the duration of one test. Saves the previous definition (if any) so
# restore_stubbed_functions can put it back -- `unset -f` would delete the production function.
STUBBED_FUNCTION_DEFS=()
STUBBED_FUNCTION_NAMES=()

stub_function() {
    local name="$1"
    local body="$2"

    if declare -F "$name" >/dev/null 2>&1; then
        STUBBED_FUNCTION_DEFS+=("$(declare -f "$name")")
    else
        STUBBED_FUNCTION_NAMES+=("$name")
    fi
    eval "${name}() { ${body} }"
}

restore_stubbed_functions() {
    local name
    # Re-eval saved definitions verbatim; iterate by index to keep multi-line bodies intact.
    local i
    for ((i = 0; i < ${#STUBBED_FUNCTION_DEFS[@]}; i++)); do
        eval "${STUBBED_FUNCTION_DEFS[$i]}"
    done
    for name in ${STUBBED_FUNCTION_NAMES+"${STUBBED_FUNCTION_NAMES[@]}"}; do
        unset -f "$name"
    done
    STUBBED_FUNCTION_DEFS=()
    STUBBED_FUNCTION_NAMES=()
}

count_log_lines() {
    local log_file="$1"
    local pattern="$2"

    awk -v pattern="$pattern" 'index($0, pattern) { count++ } END { print count + 0 }' "$log_file"
}

count_exact_log_lines() {
    local log_file="$1"
    local expected="$2"

    awk -v expected="$expected" '$0 == expected { count++ } END { print count + 0 }' "$log_file"
}

# Avoid the production retry delay in these focused unit tests while recording whether
# the retry loop requested a delay.
sleep() {
    if [[ -n "${FAKE_BAZEL_SLEEP_LOG:-}" ]]; then
        printf '%s\n' "$*" >>"$FAKE_BAZEL_SLEEP_LOG"
    else
        command sleep "$@"
    fi
}

new_tmpdir() {
    mktemp -d "${TEST_TMPDIR}/bazel_evergreen_shutils.XXXXXX"
}

make_fake_bazel() {
    local tmpdir="$1"
    local fake_bazel="${tmpdir}/fake_bazel.sh"

    cat >"$fake_bazel" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

printf '%s\n' "$*" >>"$FAKE_BAZEL_LOG"

case "$*" in
    "--batch info output_base")
        echo "unexpected --batch invocation" >&2
        exit 98
        ;;
    "info output_base")
        printf '%s\n' "$FAKE_BAZEL_OUTPUT_BASE"
        ;;
    "info command_log")
        printf '%s\n' "$FAKE_BAZEL_COMMAND_LOG"
        ;;
    "info")
        if [[ -n "${FAKE_BAZEL_PIDFILE:-}" ]]; then
            mkdir -p "$(dirname "$FAKE_BAZEL_PIDFILE")"
            printf '%s\n' "$FAKE_BAZEL_SERVER_PID" >"$FAKE_BAZEL_PIDFILE"
        fi
        ;;
    cquery*)
        [[ "$*" == *"--build_tag_filters=resmoke_config"* ]] || {
            echo "resmoke config cquery must filter by tag: $*" >&2
            exit 96
        }
        printf '%s\n' "//buildscripts/resmokeconfig:fake_config bazel-out/fake/config.yml"
        ;;
    "shutdown")
        if [[ -n "${FAKE_BAZEL_PIDFILE:-}" ]]; then
            rm -f "$FAKE_BAZEL_PIDFILE"
        fi
        ;;
    "build //evergreen:fake_target"*)
        attempt=1
        if [[ -n "${FAKE_BAZEL_BUILD_COUNT_FILE:-}" ]]; then
            if [[ -f "$FAKE_BAZEL_BUILD_COUNT_FILE" ]]; then
                attempt=$(<"$FAKE_BAZEL_BUILD_COUNT_FILE")
                attempt=$((attempt + 1))
            fi
            printf '%s\n' "$attempt" >"$FAKE_BAZEL_BUILD_COUNT_FILE"
        fi

        case "${FAKE_BAZEL_BUILD_MODE:-success}" in
            success)
                exit 0
                ;;
            fail_once)
                [[ "$attempt" -eq 1 ]] && exit 7
                exit 0
                ;;
            server_dies_once)
                if [[ "$attempt" -eq 1 ]]; then
                    rm -f "$FAKE_BAZEL_PIDFILE"
                    exit 7
                fi
                exit 0
                ;;
            always_fail)
                exit 7
                ;;
            *)
                echo "unexpected fake build mode: $FAKE_BAZEL_BUILD_MODE" >&2
                exit 97
                ;;
        esac
        ;;
    *)
        echo "unexpected bazel args: $*" >&2
        exit 99
        ;;
esac
EOF

    chmod +x "$fake_bazel"
    printf '%s\n' "$fake_bazel"
}

setup_retry_test() {
    local initial_server_state="${1:-running}"

    RETRY_TMPDIR="$(new_tmpdir)"
    export FAKE_BAZEL_LOG="${RETRY_TMPDIR}/invocations.log"
    export FAKE_BAZEL_OUTPUT_BASE="${RETRY_TMPDIR}/output-base"
    export FAKE_BAZEL_COMMAND_LOG="${RETRY_TMPDIR}/command.log"
    export FAKE_BAZEL_PIDFILE="${FAKE_BAZEL_OUTPUT_BASE}/server/server.pid.txt"
    export FAKE_BAZEL_SERVER_PID="$$"
    export FAKE_BAZEL_BUILD_COUNT_FILE="${RETRY_TMPDIR}/build-count.txt"
    export FAKE_BAZEL_BUILD_MODE="success"
    export FAKE_BAZEL_SLEEP_LOG="${RETRY_TMPDIR}/sleep.log"
    : >"$FAKE_BAZEL_LOG"
    : >"$FAKE_BAZEL_COMMAND_LOG"
    : >"$FAKE_BAZEL_SLEEP_LOG"
    mkdir -p "$(dirname "$FAKE_BAZEL_PIDFILE")"
    if [[ "$initial_server_state" == "running" ]]; then
        printf '%s\n' "$FAKE_BAZEL_SERVER_PID" >"$FAKE_BAZEL_PIDFILE"
    fi
    RETRY_FAKE_BAZEL="$(make_fake_bazel "$RETRY_TMPDIR")"

    BAZEL_EVERGREEN_OUTPUT_BASE=""
    RETRY_ON_FAIL=0
    evergreen_remote_exec=""
    build_timeout_seconds=""
    last_command_log_path=""
    env=""
    RETRY_OUTPUT=""
    RETRY_STATUS=0
}

run_retry_test_command() {
    local attempts="$1"

    RETRY_STATUS=0
    RETRY_OUTPUT="$(bazel_evergreen_shutils::retry_bazel_cmd "$attempts" "$RETRY_FAKE_BAZEL" build //evergreen:fake_target 2>&1)" || RETRY_STATUS=$?
}

test_pid_is_live_detects_live_and_dead_pids_on_posix() {
    stub_function bazel_evergreen_shutils::is_windows "return 1;"

    local status=0
    bazel_evergreen_shutils::pid_is_live "$$" || status=$?
    assert_eq "0" "$status" "the current process should be reported as live"

    status=0
    bazel_evergreen_shutils::pid_is_live "notapid" || status=$?
    assert_eq "1" "$status" "a non-numeric PID should be reported as dead"

    restore_stubbed_functions
}

test_pid_is_live_uses_tasklist_on_windows() {
    stub_function bazel_evergreen_shutils::is_windows "return 0;"
    # Emulate the Win32 tool: it lists the PID when the process exists and prints an INFO line
    # otherwise. A bash function satisfies the `command -v tasklist` probe.
    stub_function tasklist '
        if [[ "$*" == *"PID eq 4242"* ]]; then
            echo "java.exe                      4242 Services                   0    2,000,000 K"
        else
            echo "INFO: No tasks are running which match the specified criteria."
        fi
    '

    local status=0
    bazel_evergreen_shutils::pid_is_live 4242 || status=$?
    assert_eq "0" "$status" "tasklist hit should report the server as live"

    status=0
    bazel_evergreen_shutils::pid_is_live 5353 || status=$?
    assert_eq "1" "$status" "tasklist miss should report the server as dead"

    restore_stubbed_functions
}

test_pid_is_live_reports_unknown_when_tasklist_fails() {
    stub_function bazel_evergreen_shutils::is_windows "return 0;"
    # tasklist is present but errors out (access denied, a wedged WMI service, ...). Its own exit
    # status must be consulted: piping into grep would silently turn this into "no match", i.e.
    # a positively dead verdict, and shut down a healthy server.
    stub_function tasklist '
        echo "ERROR: The RPC server is unavailable." >&2
        return 1
    '

    local status=0
    bazel_evergreen_shutils::pid_is_live 4242 || status=$?
    assert_eq "2" "$status" "a failing tasklist should report liveness as undetermined"

    restore_stubbed_functions
}

test_pid_is_live_reports_unknown_when_it_cannot_check() {
    stub_function bazel_evergreen_shutils::is_windows "return 0;"
    # No tasklist available: liveness is undeterminable and must not be reported as death.
    stub_function command '
        if [[ "$*" == "-v tasklist" ]]; then
            return 1
        fi
        builtin command "$@"
    '

    local status=0
    bazel_evergreen_shutils::pid_is_live 4242 || status=$?
    assert_eq "2" "$status" "an unavailable tasklist should report liveness as undetermined"

    restore_stubbed_functions
}

# Regression test for the Windows failure where server.pid.txt holds a native Win32 PID that
# `kill -0` cannot see. The retry loop used to read that as a server death, run `bazel shutdown`
# on a healthy server, and corrupt in-flight external repository fetches (py_host), which then
# failed with "libcrypto-3-x64.dll (Permission denied)" on the next attempt.
test_retry_bazel_cmd_does_not_shutdown_server_when_liveness_is_undetermined() {
    setup_retry_test running
    export FAKE_BAZEL_BUILD_MODE="fail_once"
    RETRY_ON_FAIL=1
    stub_function bazel_evergreen_shutils::pid_is_live "return 2;"

    run_retry_test_command 2

    restore_stubbed_functions

    assert_eq "0" "$RETRY_STATUS" "undetermined liveness should still retry to success"
    assert_eq "0" "$(count_exact_log_lines "$FAKE_BAZEL_LOG" "shutdown")" "undetermined liveness must not shut down a possibly-healthy server"
    assert_eq "0" "$(count_log_lines "$FAKE_BAZEL_LOG" "--local_resources=cpu=HOST_CPUS*.5")" "undetermined liveness must not apply the OOM guard"
    assert_not_contains "$RETRY_OUTPUT" "OOM/killed" "undetermined liveness must not be diagnosed as an OOM"
}

test_cache_bazel_output_base_uses_plain_info_once() {
    local tmpdir
    local fake_bazel
    local first_output_base
    local second_output_base
    local -a invocations=()

    tmpdir="$(new_tmpdir)"
    export FAKE_BAZEL_LOG="${tmpdir}/invocations.log"
    export FAKE_BAZEL_OUTPUT_BASE="${tmpdir}/output-base"
    export FAKE_BAZEL_COMMAND_LOG="${tmpdir}/command.log"
    : >"$FAKE_BAZEL_LOG"
    : >"$FAKE_BAZEL_COMMAND_LOG"
    fake_bazel="$(make_fake_bazel "$tmpdir")"

    BAZEL_EVERGREEN_OUTPUT_BASE=""
    bazel_evergreen_shutils::cache_bazel_output_base "$fake_bazel"

    first_output_base="$(bazel_evergreen_shutils::bazel_output_base "$fake_bazel")"
    second_output_base="$(bazel_evergreen_shutils::bazel_output_base "$fake_bazel")"

    assert_eq "$FAKE_BAZEL_OUTPUT_BASE" "$BAZEL_EVERGREEN_OUTPUT_BASE" "cache helper should populate the shared output_base"
    assert_eq "$FAKE_BAZEL_OUTPUT_BASE" "$first_output_base" "first output_base lookup should use the cached value"
    assert_eq "$FAKE_BAZEL_OUTPUT_BASE" "$second_output_base" "second output_base lookup should use the cached value"

    mapfile -t invocations <"$FAKE_BAZEL_LOG"
    assert_eq "1" "${#invocations[@]}" "cache helper should only invoke bazel once"
    assert_eq "info output_base" "${invocations[0]}" "output_base lookup should use plain bazel info"
}

test_should_disable_gdb_index_for_all_ci_builds() {
    for task_name_to_check in archive_dist_test archive_dist_test_debug unit_tests; do
        if ! bazel_evergreen_shutils::should_disable_gdb_index "$task_name_to_check"; then
            fail "${task_name_to_check} should disable GDB index generation in CI"
        fi
    done
}

test_remote_unittest_wrapper_is_test_scoped() {
    local bazelrc="${TEST_SRCDIR}/${TEST_WORKSPACE}/.bazelrc"
    local common_run_under
    local test_run_under

    [[ -f "$bazelrc" ]] || fail "the repository .bazelrc should be available to this test"

    common_run_under="$(awk '$1 == "common:remote_unittest" && $2 ~ /^--run_under=/ { print }' "$bazelrc")"
    test_run_under="$(awk '$1 == "test:remote_unittest" && $2 ~ /^--run_under=/ { print }' "$bazelrc")"

    assert_eq "" "$common_run_under" "the test wrapper should not apply to bazel run"
    assert_eq \
        "test:remote_unittest --run_under=//bazel:test_wrapper" \
        "$test_run_under" \
        "the test wrapper should still apply to bazel test"
}

test_query_resmoke_configs_filters_target_universe() {
    local tmpdir
    local fake_bazel
    local output_file

    tmpdir="$(new_tmpdir)"
    export FAKE_BAZEL_LOG="${tmpdir}/invocations.log"
    export FAKE_BAZEL_OUTPUT_BASE="${tmpdir}/output-base"
    export FAKE_BAZEL_COMMAND_LOG="${tmpdir}/command.log"
    : >"$FAKE_BAZEL_LOG"
    : >"$FAKE_BAZEL_COMMAND_LOG"
    fake_bazel="$(make_fake_bazel "$tmpdir")"
    output_file="${tmpdir}/resmoke_suite_configs.yml"

    bazel_evergreen_shutils::query_resmoke_configs "$fake_bazel" "" "$output_file"

    assert_contains "$(<"$FAKE_BAZEL_LOG")" "--build_tag_filters=resmoke_config" \
        "resmoke config discovery should filter cquery's target universe"
    assert_eq \
        "//buildscripts/resmokeconfig:fake_config bazel-out/fake/config.yml" \
        "$(<"$output_file")" \
        "resmoke config discovery should preserve cquery output"
}

test_provenance_build_invocation_file_selection() {
    assert_eq \
        ".bazel_provenance_build_invocation" \
        "$(bazel_evergreen_shutils::get_provenance_build_invocation_file archive_dist_test mongodb-mongo-v8.3-staging)" \
        "archive_dist_test should create the provenance invocation file"
    assert_eq \
        ".bazel_provenance_build_invocation" \
        "$(bazel_evergreen_shutils::get_provenance_build_invocation_file package mongo-release)" \
        "release package should create the provenance invocation file"
    assert_eq \
        "" \
        "$(bazel_evergreen_shutils::get_provenance_build_invocation_file package mongodb-mongo-v8.3-staging)" \
        "non-server-release package should not create the provenance invocation file"
    assert_eq \
        ".bazel_crypt_build_invocation" \
        "$(bazel_evergreen_shutils::get_provenance_build_invocation_file crypt_create_lib mongo-release)" \
        "crypt_create_lib should create its dedicated invocation file"
    assert_eq \
        "" \
        "$(bazel_evergreen_shutils::get_provenance_build_invocation_file unit_tests mongo-release)" \
        "unrelated tasks should not create a provenance invocation file"
}

test_timeout_prefix_uses_the_expected_fallback_for_each_execution_mode() {
    local tmpdir
    local timeout_bin
    local old_path
    local local_timeout
    local local_test_timeout
    local remote_timeout
    local explicit_timeout
    local kill_after_seconds="${BAZEL_EVG_TIMEOUT_KILL_AFTER_SECONDS:-15}"

    tmpdir="$(new_tmpdir)"
    timeout_bin="${tmpdir}/timeout"
    printf '%s\n' '#!/usr/bin/env bash' 'exit 0' >"$timeout_bin"
    chmod +x "$timeout_bin"

    old_path="$PATH"
    PATH="${tmpdir}:${PATH}"

    local evergreen_remote_exec=""
    local build_timeout_seconds=""
    local_timeout="$(bazel_evergreen_shutils::timeout_prefix "$evergreen_remote_exec" "build")"
    assert_eq \
        "timeout -s QUIT -k ${kill_after_seconds}s 7200" \
        "$local_timeout" \
        "local execution should use a two-hour fallback timeout"

    local_test_timeout="$(bazel_evergreen_shutils::timeout_prefix "$evergreen_remote_exec" "test")"
    assert_eq \
        "" \
        "$local_test_timeout" \
        "local test execution should not use the build fallback timeout"

    evergreen_remote_exec="on"
    remote_timeout="$(bazel_evergreen_shutils::timeout_prefix "$evergreen_remote_exec" "build")"
    assert_eq \
        "timeout -s QUIT -k ${kill_after_seconds}s 3600" \
        "$remote_timeout" \
        "remote execution should retain its one-hour fallback timeout"

    evergreen_remote_exec=""
    build_timeout_seconds="1234"
    explicit_timeout="$(bazel_evergreen_shutils::timeout_prefix "$evergreen_remote_exec" "test")"
    assert_eq \
        "timeout -s QUIT -k ${kill_after_seconds}s 1234" \
        "$explicit_timeout" \
        "an explicit timeout should override the execution-mode fallback"

    PATH="$old_path"
}

test_retry_bazel_cmd_does_not_retry_test_timeouts() {
    local tmpdir
    local fake_bazel
    local timeout_bin
    local old_path

    setup_retry_test running
    tmpdir="$RETRY_TMPDIR"
    fake_bazel="$RETRY_FAKE_BAZEL"
    timeout_bin="${tmpdir}/timeout"
    export FAKE_TIMEOUT_LOG="${tmpdir}/timeout.log"
    : >"$FAKE_TIMEOUT_LOG"
    printf '%s\n' '#!/usr/bin/env bash' 'printf "%s\\n" "$*" >>"$FAKE_TIMEOUT_LOG"' 'exit 124' >"$timeout_bin"
    chmod +x "$timeout_bin"

    old_path="$PATH"
    PATH="${tmpdir}:${PATH}"
    build_timeout_seconds="1"

    RETRY_OUTPUT="$(bazel_evergreen_shutils::retry_bazel_cmd 3 "$fake_bazel" test //evergreen:fake_target 2>&1)" || RETRY_STATUS=$?

    assert_eq "124" "$RETRY_STATUS" "a test timeout should retain its timeout status"
    assert_eq "1" "$(awk 'END {print NR}' "$FAKE_TIMEOUT_LOG")" "a test timeout should not start another attempt"
    assert_not_contains "$RETRY_OUTPUT" "Attempt 2/3" "a test timeout should not retry"

    PATH="$old_path"
}

test_compute_local_arg_keeps_cross_rbe_for_ibm_run_mode() {
    local ppc_args
    local s390x_args

    ppc_args="$({
        evergreen_remote_exec="on"
        bazel_evergreen_shutils::bazel_rbe_supported() { return 1; }
        bazel_evergreen_shutils::is_ppc64le() { return 0; }
        bazel_evergreen_shutils::is_s390x() { return 1; }
        bazel_evergreen_shutils::compute_local_arg run
    })"
    assert_contains "$ppc_args" "--local_resources=cpu=48" "PPC run mode should retain its local CPU limit"
    assert_not_contains "$ppc_args" "--jobs=" "PPC cross-RBE should retain the common remote job limit"
    assert_not_contains "$ppc_args" "--config=local" "PPC run mode should retain cross-RBE configuration"

    s390x_args="$({
        evergreen_remote_exec="on"
        bazel_evergreen_shutils::bazel_rbe_supported() { return 1; }
        bazel_evergreen_shutils::is_ppc64le() { return 1; }
        bazel_evergreen_shutils::is_s390x() { return 0; }
        bazel_evergreen_shutils::compute_local_arg run
    })"
    assert_contains "$s390x_args" "--local_resources=cpu=16" "s390x run mode should retain its local CPU limit"
    assert_not_contains "$s390x_args" "--jobs=" "s390x cross-RBE should retain the common remote job limit"
    assert_not_contains "$s390x_args" "--config=local" "s390x run mode should retain cross-RBE configuration"
}

test_compute_local_arg_uses_local_for_non_rbe_run_mode() {
    local remote_unsupported_args
    local remote_disabled_args

    remote_unsupported_args="$({
        evergreen_remote_exec="on"
        bazel_evergreen_shutils::bazel_rbe_supported() { return 1; }
        bazel_evergreen_shutils::is_ppc64le() { return 1; }
        bazel_evergreen_shutils::is_s390x() { return 1; }
        bazel_evergreen_shutils::compute_local_arg run
    })"
    assert_contains "$remote_unsupported_args" "--config=local" "unsupported run hosts should use local configuration"

    remote_disabled_args="$({
        evergreen_remote_exec="off"
        bazel_evergreen_shutils::bazel_rbe_supported() { return 0; }
        bazel_evergreen_shutils::is_ppc64le() { return 1; }
        bazel_evergreen_shutils::is_s390x() { return 1; }
        bazel_evergreen_shutils::compute_local_arg run
    })"
    assert_contains "$remote_disabled_args" "--config=local" "run mode should use local configuration when remote execution is disabled"
}

test_maybe_release_flag_classifies_patch_test_and_release_tasks() {
    local output

    output="$({
        MONGO_VERSION_OVERRIDE=""
        is_patch="true"
        release_rbe="false"
        push_bucket="downloads.example.invalid"
        compiling_for_test="false"
        bazel_evergreen_shutils::maybe_release_flag "--config=evg"
    })"
    assert_not_contains "$output" "public-release" "patch builds should not select a release config"

    output="$({
        MONGO_VERSION_OVERRIDE=""
        is_patch="false"
        release_rbe="false"
        push_bucket="downloads.example.invalid"
        compiling_for_test="true"
        bazel_evergreen_shutils::maybe_release_flag "--config=evg"
    })"
    assert_not_contains "$output" "public-release" "test tasks should not select a release config"

    output="$({
        MONGO_VERSION_OVERRIDE=""
        is_patch="false"
        release_rbe="false"
        push_bucket="downloads.example.invalid"
        compiling_for_test="false"
        bazel_evergreen_shutils::maybe_release_flag "--config=evg"
    })"
    assert_contains "$output" "--config=public-release-local" "release artifacts should use local release mode"

    output="$({
        MONGO_VERSION_OVERRIDE=""
        is_patch="false"
        release_rbe="true"
        push_bucket="downloads.example.invalid"
        compiling_for_test="false"
        bazel_evergreen_shutils::maybe_release_flag "--config=evg"
    })"
    assert_contains "$output" "--config=public-release-rbe" "explicit release RBE should remain enabled"
}

test_local_release_command_disables_remote_fallback_timeout() {
    if ! bazel_evergreen_shutils::command_uses_local_release \
        build //evergreen:fake_target --config=public-release-local; then
        fail "public-release-local should disable the remote fallback timeout"
    fi
    if ! bazel_evergreen_shutils::command_uses_local_release \
        build //evergreen:fake_target --remote_executor=; then
        fail "an empty remote executor should disable the remote fallback timeout"
    fi
    if bazel_evergreen_shutils::command_uses_local_release \
        build //evergreen:fake_target --config=evg; then
        fail "ordinary test commands should retain the remote fallback timeout"
    fi
}

test_retry_bazel_cmd_primes_output_base_before_running_bazel() {
    local tmpdir
    local fake_bazel
    local -a invocations=()

    tmpdir="$(new_tmpdir)"
    export FAKE_BAZEL_LOG="${tmpdir}/invocations.log"
    export FAKE_BAZEL_OUTPUT_BASE="${tmpdir}/output-base"
    export FAKE_BAZEL_COMMAND_LOG="${tmpdir}/command.log"
    : >"$FAKE_BAZEL_LOG"
    : >"$FAKE_BAZEL_COMMAND_LOG"
    mkdir -p "${FAKE_BAZEL_OUTPUT_BASE}/server"
    printf '%s\n' "$$" >"${FAKE_BAZEL_OUTPUT_BASE}/server/server.pid.txt"
    fake_bazel="$(make_fake_bazel "$tmpdir")"

    BAZEL_EVERGREEN_OUTPUT_BASE=""
    RETRY_ON_FAIL=0
    evergreen_remote_exec=""
    build_timeout_seconds=""
    last_command_log_path=""
    env=""

    bazel_evergreen_shutils::retry_bazel_cmd 1 "$fake_bazel" build //evergreen:fake_target

    assert_eq "$FAKE_BAZEL_OUTPUT_BASE" "$BAZEL_EVERGREEN_OUTPUT_BASE" "retry wrapper should cache output_base up front"

    mapfile -t invocations <"$FAKE_BAZEL_LOG"
    assert_eq "3" "${#invocations[@]}" "retry wrapper should only query output_base, command_log, and the build"
    assert_eq "info output_base" "${invocations[0]}" "retry wrapper should cache output_base before other bazel calls"
    assert_eq "info command_log" "${invocations[1]}" "retry wrapper should still record the command log path"
    assert_eq "build //evergreen:fake_target" "${invocations[2]}" "retry wrapper should run the requested bazel command"
}

test_retry_bazel_cmd_reuses_healthy_server_after_regular_failure() {
    setup_retry_test running
    export FAKE_BAZEL_BUILD_MODE="fail_once"
    RETRY_ON_FAIL=1

    run_retry_test_command 2

    assert_eq "0" "$RETRY_STATUS" "regular failure should be retried successfully"
    assert_eq "2" "$(count_log_lines "$FAKE_BAZEL_LOG" "build //evergreen:fake_target")" "regular failure should run the build twice"
    assert_eq "0" "$(count_exact_log_lines "$FAKE_BAZEL_LOG" "shutdown")" "regular failure should reuse the healthy server"
    assert_eq "1" "$(count_log_lines "$FAKE_BAZEL_SLEEP_LOG" "60")" "regular failure should retain the retry backoff"
    assert_contains "$RETRY_OUTPUT" "Bazel failed (exit=7); retrying with existing server..." "regular failure should explain that the server is reused"
    assert_not_contains "$RETRY_OUTPUT" "OOM/killed" "regular failure should not be diagnosed as an OOM"
}

test_retry_bazel_cmd_starts_missing_server_with_neutral_message() {
    setup_retry_test missing

    run_retry_test_command 1

    assert_eq "0" "$RETRY_STATUS" "build should succeed after starting a missing server"
    assert_eq "1" "$(count_exact_log_lines "$FAKE_BAZEL_LOG" "info")" "missing server should be started once"
    assert_contains "$RETRY_OUTPUT" "Bazel server not running; starting…" "pre-attempt server startup should use neutral logging"
    assert_not_contains "$RETRY_OUTPUT" "likely OOM/killed" "missing pre-attempt server should not be diagnosed as an OOM"
}

test_retry_bazel_cmd_restarts_server_after_unexpected_death() {
    setup_retry_test running
    export FAKE_BAZEL_BUILD_MODE="server_dies_once"
    RETRY_ON_FAIL=1

    run_retry_test_command 2

    assert_eq "0" "$RETRY_STATUS" "server death should be recovered on the next attempt"
    assert_eq "1" "$(count_exact_log_lines "$FAKE_BAZEL_LOG" "shutdown")" "server death should clean up the old server state"
    assert_eq "1" "$(count_exact_log_lines "$FAKE_BAZEL_LOG" "info")" "server death should restart the server"
    assert_eq "1" "$(count_log_lines "$FAKE_BAZEL_LOG" "--local_resources=cpu=HOST_CPUS*.5")" "server death should apply the OOM guard"
    assert_contains "$RETRY_OUTPUT" "Bazel server exited unexpectedly (possibly OOM/killed). Enabling OOM guard for next attempt and restarting…" "unexpected server death should be diagnosed after the attempt"
}

test_retry_bazel_cmd_fails_fast_when_regular_retries_are_disabled() {
    setup_retry_test running
    export FAKE_BAZEL_BUILD_MODE="always_fail"
    RETRY_ON_FAIL=0

    run_retry_test_command 3

    assert_eq "7" "$RETRY_STATUS" "regular failure should retain its exit status"
    assert_eq "1" "$(count_log_lines "$FAKE_BAZEL_LOG" "build //evergreen:fake_target")" "disabled retries should run the build once"
    assert_eq "0" "$(count_exact_log_lines "$FAKE_BAZEL_LOG" "shutdown")" "disabled retries should not restart the server"
    assert_eq "0" "$(count_log_lines "$FAKE_BAZEL_SLEEP_LOG" "60")" "disabled retries should not sleep"
}

test_retry_bazel_cmd_does_not_retry_or_sleep_after_final_failure() {
    setup_retry_test running
    export FAKE_BAZEL_BUILD_MODE="always_fail"
    RETRY_ON_FAIL=1

    run_retry_test_command 1

    assert_eq "7" "$RETRY_STATUS" "final failure should retain its exit status"
    assert_eq "1" "$(count_log_lines "$FAKE_BAZEL_LOG" "build //evergreen:fake_target")" "final failure should run the build once"
    assert_eq "0" "$(count_exact_log_lines "$FAKE_BAZEL_LOG" "shutdown")" "final failure should not restart the server"
    assert_eq "0" "$(count_log_lines "$FAKE_BAZEL_SLEEP_LOG" "60")" "final failure should not sleep"
    assert_not_contains "$RETRY_OUTPUT" "retrying" "final failure should not claim that another retry will run"

    setup_retry_test running
    export FAKE_BAZEL_BUILD_MODE="server_dies_once"
    RETRY_ON_FAIL=1

    run_retry_test_command 1

    assert_eq "7" "$RETRY_STATUS" "final server-death failure should retain its exit status"
    assert_eq "0" "$(count_exact_log_lines "$FAKE_BAZEL_LOG" "shutdown")" "final server-death failure should not clean up for a nonexistent retry"
    assert_eq "0" "$(count_exact_log_lines "$FAKE_BAZEL_LOG" "info")" "final server-death failure should not restart the server"
    assert_eq "0" "$(count_log_lines "$FAKE_BAZEL_SLEEP_LOG" "60")" "final server-death failure should not sleep"
    assert_not_contains "$RETRY_OUTPUT" "next attempt" "final server-death failure should not claim that another retry will run"
}

test_pid_is_live_detects_live_and_dead_pids_on_posix
test_pid_is_live_uses_tasklist_on_windows
test_pid_is_live_reports_unknown_when_it_cannot_check
test_pid_is_live_reports_unknown_when_tasklist_fails
test_retry_bazel_cmd_does_not_shutdown_server_when_liveness_is_undetermined
test_cache_bazel_output_base_uses_plain_info_once
test_should_disable_gdb_index_for_all_ci_builds
test_remote_unittest_wrapper_is_test_scoped
test_query_resmoke_configs_filters_target_universe
test_provenance_build_invocation_file_selection
test_timeout_prefix_uses_the_expected_fallback_for_each_execution_mode
test_retry_bazel_cmd_does_not_retry_test_timeouts
test_compute_local_arg_keeps_cross_rbe_for_ibm_run_mode
test_compute_local_arg_uses_local_for_non_rbe_run_mode
test_maybe_release_flag_classifies_patch_test_and_release_tasks
test_local_release_command_disables_remote_fallback_timeout
test_retry_bazel_cmd_primes_output_base_before_running_bazel
test_retry_bazel_cmd_reuses_healthy_server_after_regular_failure
test_retry_bazel_cmd_starts_missing_server_with_neutral_message
test_retry_bazel_cmd_restarts_server_after_unexpected_death
test_retry_bazel_cmd_fails_fast_when_regular_retries_are_disabled
test_retry_bazel_cmd_does_not_retry_or_sleep_after_final_failure
