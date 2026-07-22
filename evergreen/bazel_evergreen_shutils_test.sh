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

test_cache_bazel_output_base_uses_plain_info_once
test_retry_bazel_cmd_primes_output_base_before_running_bazel
test_retry_bazel_cmd_reuses_healthy_server_after_regular_failure
test_retry_bazel_cmd_starts_missing_server_with_neutral_message
test_retry_bazel_cmd_restarts_server_after_unexpected_death
test_retry_bazel_cmd_fails_fast_when_regular_retries_are_disabled
test_retry_bazel_cmd_does_not_retry_or_sleep_after_final_failure
