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
    "build //evergreen:fake_target")
        exit 0
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

test_cache_bazel_output_base_uses_plain_info_once
test_retry_bazel_cmd_primes_output_base_before_running_bazel
