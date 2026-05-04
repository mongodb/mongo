# Executes resmoke suite bazel test targets.
#
# Usage:
#   bash resmoke_tests_execute_bazel.sh
#
# Required environment variables:
# * ${targets} - Resmoke bazel target, like //buildscripts/resmokeconfig:core

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"
. "$DIR/bazel_evergreen_shutils.sh"

# Result tasks re-invoke this script to conditionally re-execute the test. The test should
# execute unless the task was activated by the resmoke_tests task that already ran all tests.
exit_early_if_result_task() {
    if [[ -f "src/build_events.json" && "$activated_by" == "mongodb-mongo-ci-user" ]]; then
        echo "Tests were executed by the resmoke_tests task, test results will be fetched from their remote execution."
        exit 0
    fi
}

# Interprets the final bazel return code for the runner task.
# Return code 3 from `bazel test` indicates that the build was OK, but some tests failed or timed out.
# The test failures are reported in individual results tasks, so don't fail the task here.
exit_for_runner_task() {
    local ret=$1
    if [[ "$ret" -eq 3 ]]; then
        echo 'Some tests failed. See the generated task(s) for the failed targets for more details on the failure(s).'
        exit 0
    elif [[ "$ret" -eq 4 ]]; then
        # Before suites are converted, this is expected and should not fail the task. Remove with SERVER-118686.
        echo 'No tests were run.'
        exit 0
    elif [[ "$ret" -eq 0 ]]; then
        exit 0
    else
        echo 'Some tests failed to build. Look for "FAILED TO BUILD" or other build errors above. Tests with regular test failures will have their results in separate generated tasks.'
        exit "$ret"
    fi
}

exit_for_result_task() {
    local ret=$1
    if [[ "$ret" -eq 3 ]]; then
        echo 'Some tests failed, the task will be failed after fetching test results.'
        exit 0
    else
        exit "$ret"
    fi
}

build_ci_flags() {
    ci_flags="--//bazel/resmoke:in_evergreen"

    # For simple build ID generation:
    export compile_variant="${compile_variant}"
    export version_id="${version_id}"

    if [[ "${evergreen_remote_exec}" == "on" ]]; then
        ci_flags="--config=remote_test ${ci_flags}"
    fi

    if [ "${should_shuffle}" = true ]; then
        ci_flags+=" --test_arg=--shuffle"
    elif [ "${should_shuffle}" = false ]; then
        ci_flags+=" --test_arg=--shuffleMode=off"
    fi

    if [ "${is_patch}" = "true" ]; then
        ci_flags+=" --test_arg=--patchBuild"
    fi

    if [ "${skip_symbolization}" = "true" ]; then
        ci_flags+=" --test_arg=--skipSymbolization"
    fi

    if [ "${enable_evergreen_api_test_selection}" = "true" ]; then
        ci_flags+=" --test_arg=--enableEvergreenApiTestSelection"
    fi

    # Split comma separated list of strategies
    IFS=',' read -a strategies <<<"$test_selection_strategies_array"
    for strategy in "${strategies[@]}"; do
        ci_flags+=" --test_arg=--evergreenTestSelectionStrategy=${strategy}"
    done

    # Add each test flag from test_flags expansion as --test_arg
    if [ -n "${test_flags:-}" ]; then
        eval "flags_array=(${test_flags})"
        for flag in "${flags_array[@]}"; do
            bazel_args+=" --test_arg=\"${flag}\""
        done
    fi
}

save_invocation() {
    # Save the invocation, intentionally excluding CI specific flags.
    echo "python buildscripts/install_bazel.py" >bazel-invocation.txt
    echo "bazel test ${bazel_args} ${targets}" >>bazel-invocation.txt
}

maybe_generate_burn_in_targets() {
    if [ "${generate_burn_in_targets}" != "true" ]; then
        return
    fi
    echo "Generating burn-in test targets..."
    base_revision="$(git merge-base ${revision} HEAD)"
    ${BAZEL_BINARY} build ${CONFIG_FLAGS} //... --build_tag_filters=resmoke_config
    bazel_evergreen_shutils::query_resmoke_configs "${BAZEL_BINARY}" "${CONFIG_FLAGS}" "resmoke_suite_configs.yml"
    ${BAZEL_BINARY} run ${CONFIG_FLAGS} //buildscripts:bazel_burn_in -- generate-targets "$base_revision" || echo "Failed to generate burn-in targets"
}

# Fetches then tests with retries. Leaves the result in the global RET.
run_fetch_and_test() {
    export RETRY_ON_FAIL=1
    bazel_evergreen_shutils::retry_bazel_cmd 3 "$BAZEL_BINARY" \
        fetch ${ci_flags} ${bazel_args} ${bazel_compile_flags} ${task_compile_flags} ${patch_compile_flags} ${targets}
    RET=$?

    if [[ "$RET" != "0" ]]; then
        return
    fi

    export RETRY_ON_FAIL=0
    bazel_evergreen_shutils::retry_bazel_cmd 2 "$BAZEL_BINARY" \
        test ${ci_flags} ${bazel_args} ${bazel_compile_flags} ${task_compile_flags} ${patch_compile_flags} --build_event_json_file=build_events.json ${targets}
    RET=$?

    if [[ "$RET" -eq 124 ]]; then
        echo "Bazel timed out after ${build_timeout_seconds:-<unspecified>} seconds."
    elif [[ "$RET" != "0" ]]; then
        echo "Errors were found during bazel test, failing the execution"
    fi
}

gather_failed_tests() {
    if [[ "$RET" == "0" ]]; then
        return
    fi
    # This is a hacky way to save build time for the initial build during the `bazel test` above. They
    # are stripped binaries there. We should rebuild them with debug symbols and separate debug.
    # The relinked binaries should still be hash identical when stripped with strip.
    sed -i -e 's/--config=remote_test//g' -e 's/--separate_debug=False/--separate_debug=True/g' -e 's/--features=strip_debug//g' .bazel_build_flags

    # The --config flag needs to stay consistent for the `bazel run` to avoid evicting the previous results.
    # Strip out anything that isn't a --config flag that could interfere with the run command.
    eval ${BAZEL_BINARY} run ${CONFIG_FLAGS} //buildscripts:gather_failed_tests || true
}

activate_result_tasks() {
    if [ "${generate_burn_in_targets}" = "true" ]; then
        return
    fi
    echo "Activating result task group..."
    python buildscripts/evergreen_activate_result_tasks.py --expansion-file ../expansions.yml --build-events-file build_events.json
}

main() {
    set -o errexit
    set -o verbose

    echo Expansions: activated_by:"$activated_by" execution:"$execution" is_stepback:"$is_stepback" standalone:"$standalone"

    exit_early_if_result_task

    bazel_evergreen_shutils::activate_and_cd_src

    BAZEL_BINARY=$(bazel_evergreen_shutils::bazel_get_binary_path)
    export BAZEL_BINARY

    build_ci_flags

    ALL_FLAGS="${ci_flags} ${LOCAL_ARG} ${bazel_args:-} ${bazel_compile_flags:-} ${task_compile_flags:-} ${patch_compile_flags:-}"
    CONFIG_FLAGS="$(bazel_evergreen_shutils::extract_config_flags "${ALL_FLAGS}")"
    echo "${ALL_FLAGS}" >.bazel_build_flags

    save_invocation

    maybe_generate_burn_in_targets

    set +o errexit
    run_fetch_and_test
    bazel_evergreen_shutils::write_last_engflow_link
    set -o errexit

    if [[ -n "$result_task" ]]; then
        # Explicitly shutdown the bazel server in case the Evergreen agent is tracking it for completion of this process.
        eval ${BAZEL_BINARY} shutdown
        exit_for_result_task "$RET"
    fi

    gather_failed_tests

    activate_result_tasks

    # Explicitly shutdown the bazel server in case the Evergreen agent is tracking it for completion of this process.
    eval ${BAZEL_BINARY} shutdown
    exit_for_runner_task "$RET"
}

main "$@"
