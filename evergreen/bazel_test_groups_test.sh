#!/usr/bin/env bash

set -euo pipefail

workspace="${TEST_SRCDIR}/${TEST_WORKSPACE}"
test_root="$(mktemp -d "${TEST_TMPDIR}/bazel-test-groups-XXXXXX")"
mkdir -p "${test_root}/scripts"
cp "${workspace}/evergreen/bazel_test.sh" "${test_root}/scripts/"
touch "${test_root}/scripts/prelude.sh"
cat >"${test_root}/scripts/bazel_evergreen_shutils.sh" <<'SH'
bazel_evergreen_shutils::activate_and_cd_src() { cd "$CAPTURE_TEST_DIR"; }
bazel_evergreen_shutils::export_ssl_paths_if_needed() { :; }
bazel_evergreen_shutils::is_windows() { [[ "$CAPTURE_WINDOWS" == true ]]; }
bazel_evergreen_shutils::bazel_get_binary_path() { echo /bin/true; }
bazel_evergreen_shutils::compute_local_arg() { :; }
bazel_evergreen_shutils::maybe_release_flag() { echo "$1"; }
bazel_evergreen_shutils::maybe_scale_test_timeout_and_append() { :; }
bazel_evergreen_shutils::write_last_engflow_link() { :; }
bazel_evergreen_shutils::gather_failed_tests() {
    touch gathered
    if [[ -f build_events.json ]]; then
        cp build_events.json gathered.json
    fi
}
cygpath() { echo "$2"; }
python=fake_python
fake_python() {
    touch planned
    if [[ "$CAPTURE_PLAN_STATUS" != 0 ]]; then
        return "$CAPTURE_PLAN_STATUS"
    fi
    : >"$3"
    : >"$4"
    if [[ "$CAPTURE_GROUP_MODE" != sensitive_only ]]; then
        echo '//tests:normal' >"$3"
    fi
    if [[ "$CAPTURE_GROUP_MODE" != normal_only ]]; then
        echo '//tests:sensitive' >"$4"
    fi
}
bazel_evergreen_shutils::retry_bazel_cmd() {
    printf '%s RETRY_ON_FAIL=%s\n' "$*" "$RETRY_ON_FAIL" >>commands.txt
    local command="$3"
    if [[ "$command" == build ]]; then
        return "$CAPTURE_BUILD_STATUS"
    fi
    local status="$CAPTURE_NORMAL_STATUS" label=normal arg bep_file=""
    for arg in "$@"; do
        case "$arg" in
        --build_event_json_file=*) bep_file="${arg#*=}" ;;
        --target_pattern_file=*timing_sensitive*)
            status="$CAPTURE_TIMING_STATUS"
            label=sensitive
            ;;
        esac
    done
    local overall=PASSED
    if [[ "$status" != 0 ]]; then
        overall=FAILED
    fi
    printf '{"id":{"testSummary":{"label":"//tests:%s"}},"testSummary":{"overallStatus":"%s"}}\n' "$label" "$overall" >"$bep_file"
    return "$status"
}
SH

run_test() {
    local name="$1" windows="$2" task="$3" normal_status="$4" timing_status="$5"
    mkdir -p "${test_root}/${name}"
    result=0
    CAPTURE_TEST_DIR="${test_root}/${name}" CAPTURE_WINDOWS="$windows" \
        CAPTURE_NORMAL_STATUS="$normal_status" CAPTURE_TIMING_STATUS="$timing_status" \
        CAPTURE_BUILD_STATUS="${CAPTURE_BUILD_STATUS:-0}" \
        CAPTURE_PLAN_STATUS="${CAPTURE_PLAN_STATUS:-0}" \
        CAPTURE_GROUP_MODE="${CAPTURE_GROUP_MODE:-both}" \
        targets=//src/... bazel_args='--test_tag_filters=mongo_unittest,-intermediate_debug' \
        task_name="$task" version=9.1.0 no_mongo_version=false \
        bash "${test_root}/scripts/bazel_test.sh" >"${test_root}/${name}/log" 2>&1 || result=$?
}

read_commands() {
    commands=()
    while IFS= read -r command; do
        commands+=("$command")
    done <"${test_root}/$1/commands.txt"
}

run_test windows true unit_tests 0 0
read_commands windows
[[ "$result" == 0 && "${#commands[@]}" == 3 ]]
[[ "${commands[0]}" == *'build --build_tests_only'* ]]
[[ "${commands[0]}" == *'RETRY_ON_FAIL=1'* ]]
[[ "${commands[1]}" == *'--target_pattern_file=bazel-test-targets.normal.txt'* ]]
[[ "${commands[1]}" != *'--local_test_jobs='* ]]
[[ "${commands[2]}" == *'--target_pattern_file=bazel-test-targets.timing_sensitive.txt'* ]]
[[ "${commands[2]}" == *'--local_test_jobs=1'* ]]
[[ "${commands[1]}" == *'RETRY_ON_FAIL=0'* && "${commands[2]}" == *'RETRY_ON_FAIL=0'* ]]
[[ "${commands[1]}" == *'--test_tag_filters=mongo_unittest,-intermediate_debug'* ]]
[[ "${commands[2]}" == *'--test_tag_filters=mongo_unittest,-intermediate_debug'* ]]
[[ "$(wc -l <"${test_root}/windows/build_events.json")" == 2 ]]

run_test normal_failed true unit_tests 3 0
read_commands normal_failed
[[ "$result" == 3 && "${#commands[@]}" == 3 ]]
[[ -f "${test_root}/normal_failed/gathered" ]]
[[ "$(cat "${test_root}/normal_failed/gathered.json")" == *'"overallStatus":"FAILED"'* ]]
[[ "$(wc -l <"${test_root}/normal_failed/gathered.json")" == 2 ]]

run_test timing_failed true unit_tests 0 7
[[ "$result" == 7 && -f "${test_root}/timing_failed/gathered" ]]

run_test both_failed true unit_tests 3 7
[[ "$result" == 3 ]]

CAPTURE_GROUP_MODE=sensitive_only run_test sensitive_only true unit_tests 0 0
read_commands sensitive_only
[[ "$result" == 0 && "${#commands[@]}" == 2 ]]
[[ "${commands[1]}" == *'--local_test_jobs=1'* ]]

CAPTURE_GROUP_MODE=normal_only run_test normal_only true unit_tests 0 0
read_commands normal_only
[[ "$result" == 0 && "${#commands[@]}" == 2 ]]
[[ "${commands[1]}" == *' //src/...'* && "${commands[1]}" != *'--target_pattern_file='* ]]

run_test linux false unit_tests 0 0
read_commands linux
[[ "$result" == 0 && "${#commands[@]}" == 2 && ! -f "${test_root}/linux/planned" ]]

run_test other_task true integration_tests 0 0
read_commands other_task
[[ "$result" == 0 && "${#commands[@]}" == 2 && ! -f "${test_root}/other_task/planned" ]]

CAPTURE_BUILD_STATUS=9 run_test build_failed true unit_tests 0 0
read_commands build_failed
[[ "$result" == 9 && "${#commands[@]}" == 1 && ! -f "${test_root}/build_failed/planned" ]]

CAPTURE_PLAN_STATUS=5 run_test plan_failed true unit_tests 0 0
read_commands plan_failed
[[ "$result" == 5 && "${#commands[@]}" == 1 ]]
