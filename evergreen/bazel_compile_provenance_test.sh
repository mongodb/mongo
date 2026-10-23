#!/usr/bin/env bash

set -euo pipefail

workspace="${TEST_SRCDIR}/${TEST_WORKSPACE}"
test_root="$(mktemp -d "${TEST_TMPDIR}/compile-provenance-XXXXXX")"
mkdir -p "${test_root}/scripts"
cp "${workspace}/evergreen/bazel_compile.sh" "${test_root}/scripts/"
touch "${test_root}/scripts/prelude.sh"
cat >"${test_root}/scripts/bazel_evergreen_shutils.sh" <<'SH'
source "${TEST_SRCDIR}/${TEST_WORKSPACE}/evergreen/bazel_evergreen_shutils.sh"
bazel_evergreen_shutils::activate_and_cd_src() { cd "$CAPTURE_TEST_DIR"; }
bazel_evergreen_shutils::export_ssl_paths_if_needed() { :; }
bazel_evergreen_shutils::bazel_get_binary_path() { echo /bin/true; }
bazel_evergreen_shutils::compute_local_arg() { :; }
bazel_evergreen_shutils::check_release_distro() { :; }
bazel_evergreen_shutils::ensure_server_and_print_pid() { :; }
bazel_evergreen_shutils::write_last_engflow_link() { :; }
bazel_evergreen_shutils::is_windows() { [[ "$CAPTURE_TEST_WINDOWS" == true ]]; }
bazel_evergreen_shutils::retry_bazel_cmd() {
    printf '%s\n' "$*" >>commands.txt
    if [[ "$(wc -l <commands.txt)" == 1 ]]; then
        return "$CAPTURE_TEST_BUILD_STATUS"
    fi
    if [[ "$CAPTURE_TEST_PROBE_STATUS" != 0 ]]; then
        return "$CAPTURE_TEST_PROBE_STATUS"
    fi
    mkdir -p bazel-bin/bazel/config
    printf '{"compiler_version":"19.1.7"}\n' >bazel-bin/bazel/config/toolchain_provenance.json
}
SH

run_compile() {
    local name="$1" target="$2" flags="$3" build_status="$4" probe_status="$5" windows="$6"
    mkdir -p "${test_root}/${name}"
    result=0
    CAPTURE_TEST_DIR="${test_root}/${name}" CAPTURE_TEST_WINDOWS="$windows" \
        CAPTURE_TEST_BUILD_STATUS="$build_status" CAPTURE_TEST_PROBE_STATUS="$probe_status" \
        targets="$target" bazel_args="$flags" task_name=archive_dist_test version=9.1.0 \
        project=mongodb-mongo-master is_patch="${CAPTURE_TEST_PATCH:-true}" is_release=false \
        bash "${test_root}/scripts/bazel_compile.sh" >"${test_root}/${name}/log" 2>&1 || result=$?
}

# Report-only aspects and positive target filters omit the provenance output
# from a combined invocation. The second build must produce and copy it.
run_compile filtered //src/... '--config=symbol-checker --output_groups=report --build_tag_filters=mongo_binary --linkopt=-s' 0 0 false
[[ "$result" == 0 ]]
mapfile -t commands <"${test_root}/filtered/commands.txt"
[[ "${#commands[@]}" == 2 ]]
[[ "${commands[0]}" != *toolchain_provenance* ]]
[[ "${commands[1]}" == *'--linkopt=-s'* ]]
[[ "${commands[1]}" == *'--aspects= --output_groups=default --build_tag_filters= //bazel/config:toolchain_provenance'* ]]
cmp "${test_root}/filtered/bazel-bin/bazel/config/toolchain_provenance.json" "${test_root}/filtered/.bazel_toolchain_provenance.json"

run_compile compiledb compiledb '' 0 0 false
[[ "$result" == 0 && "$(wc -l <"${test_root}/compiledb/commands.txt")" == 1 ]]
[[ ! -f "${test_root}/compiledb/.bazel_toolchain_provenance.json" ]]

run_compile resmoke_configs //buildscripts/resmokeconfig/... '--build_tag_filters=resmoke_config --repo_env=no_c++_toolchain=1' 0 0 false
[[ "$result" == 0 && "$(wc -l <"${test_root}/resmoke_configs/commands.txt")" == 1 ]]
[[ ! -f "${test_root}/resmoke_configs/.bazel_toolchain_provenance.json" ]]

run_compile windows archive-dist-stripped '' 0 0 true
[[ "$result" == 0 && "$(wc -l <"${test_root}/windows/commands.txt")" == 1 ]]

run_compile build_failed archive-dist-stripped '' 7 0 false
[[ "$result" == 7 && "$(wc -l <"${test_root}/build_failed/commands.txt")" == 1 ]]

run_compile probe_failed archive-dist-stripped '' 0 9 false
[[ "$result" == 9 && ! -f "${test_root}/probe_failed/.bazel_toolchain_provenance.json" ]]

CAPTURE_TEST_PATCH=false run_compile release archive-dist-stripped '--config=public-release-local' 0 0 false
mapfile -t commands <"${test_root}/release/commands.txt"
[[ "$result" == 0 && "${#commands[@]}" == 2 ]]
suffix='--remote_executor= --noremote_accept_cached --remote_upload_local_results=false --modify_execution_info=.*=+no-cache'
[[ "${commands[0]}" == *"$suffix" && "${commands[1]}" == *"$suffix" ]]
[[ "${commands[0]}" == *'--execution_log_compact_file='* ]]
[[ "${commands[1]}" != *'--execution_log_compact_file='* ]]
