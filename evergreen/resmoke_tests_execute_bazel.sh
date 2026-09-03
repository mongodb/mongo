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
. "$DIR/engflow_links_shutils.sh"

if [[ "${resmoke_rbe_mirror_reenabled}" != "true" && "${build_variant}" == "enterprise-amazon-linux2023-arm64-all-feature-flags-rbe" ]]; then
    echo "Skipping: the RBE mirror variant is disabled. Set the resmoke_rbe_mirror_reenabled project variable to true to enable it. Report to #ask-devprod-test-infrastructure if you have any issues."
    exit 0
fi

# Result tasks re-invoke this script to conditionally re-execute the test. The test should
# execute unless the task was activated by the resmoke_tests task that already ran all tests.
exit_early_if_result_task() {
    if [[ "${resmoke_disable_rbe}" == "true" ]]; then
        return # Local exec: result tasks must always run bazel test themselves.
    fi
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
    elif [[ "$ret" -eq 4 ]]; then
        # No tests ran for this target (e.g. it was skipped on this platform). Mirror the runner
        # task's tolerance instead of failing the result task. Remove with SERVER-118686.
        echo 'No tests were run.'
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

    if [[ "${evergreen_remote_exec}" == "on" && "${resmoke_disable_rbe}" != "true" ]]; then
        ci_flags="--config=remote_test ${ci_flags}"
    fi

    if [[ "${resmoke_disable_rbe}" == "true" ]]; then
        ci_flags+=" --//bazel/resmoke:installed_dist_test"
    fi

    # Thread the mongot expansions to the flags consumed by
    # //bazel/resmoke/mongot:mongot-localdev for suites that run real mongot.
    if [[ "$(uname -m)" == aarch64* ]]; then
        mongot_url="${linux_aarch64_mongot_localdev_binary:-}"
    else
        mongot_url="${linux_x86_64_mongot_localdev_binary:-}"
    fi
    if [[ -n "$mongot_url" ]]; then
        # Downstream 10gen/mongot patch: use the patched mongot tarball.
        ci_flags+=" --//bazel/resmoke/mongot:localdev-url=${mongot_url}"
    elif [[ "${download_mongot_release:-}" == "true" ]]; then
        ci_flags+=" --//bazel/resmoke/mongot:version=release"
    fi

    if [ -n "${shuffle_mode:-}" ]; then
        ci_flags+=" --test_arg=--shuffleMode=${shuffle_mode}"
    elif [ "${should_shuffle}" = true ]; then
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

    # Test selection is applied ahead of the tests by the per-suite test list targets, which ask
    # the service which tests to keep while the suites are still being built. Resmoke itself is not
    # asked to select (it reads the resulting file), so the gate lives here, where the patch and
    # project settings are known. Strategies are passed through verbatim from the same patch
    # parameter the non-bazel flow uses; empty means resmoke's own default.
    tss_enabled="${enable_evergreen_api_test_selection:-${is_test_selection_enabled}}"
    if [[ "$tss_enabled" == "true" && "${is_patch}" == "true" ]]; then
        ci_flags+=" --//bazel/resmoke:enable_test_selection=True"
        ci_flags+=" --//bazel/resmoke:test_selection_strategies=${test_selection_strategies_array}"
    fi

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

# Targets whose build is deferred to the `bazel test` phase below, skipping the retried
# pre-build. Their inputs come from build actions with very long downloads (the
# query_correctness test corpora), so building them up front serializes that download ahead of
# all test execution instead of overlapping with it.
# The tradeoff is that a build failure in one of these targets is not retried.
DEFERRED_BUILD_TARGETS=(
    "//jstests/suites/query-optimization:query_correctness_generated_test_1"
    "//jstests/suites/query-optimization:query_correctness_generated_test_2"
    "//jstests/suites/query-optimization:query_correctness_generated_test_3"
    "//jstests/suites/query-optimization:query_correctness_generated_test_4"
    "//jstests/suites/query-execution:query_correctness_query_shape_hash_stability_generated_test_1"
    "//jstests/suites/query-execution:query_correctness_query_shape_hash_stability_generated_test_2"
    "//jstests/suites/query-execution:query_correctness_query_shape_hash_stability_generated_test_3"
    "//jstests/suites/query-execution:query_correctness_query_shape_hash_stability_generated_test_4"
)

targets_for_build_phase() {
    local target deferred kept=0
    for target in ${targets}; do
        for deferred in "${DEFERRED_BUILD_TARGETS[@]}"; do
            if [[ "$target" == "$deferred" ]]; then
                continue 2
            fi
        done
        printf '%s ' "$target"
        kept=1
    done

    if [[ "$kept" == "0" ]]; then
        return
    fi

    for deferred in "${DEFERRED_BUILD_TARGETS[@]}"; do
        printf -- '-%s ' "$deferred"
    done
}

STREAMED_TASKS_FILE="streamed_result_tasks.txt"

# Start the process that tails build_events.json and activates each result task
# as soon as its target's remote execution completes, instead of waiting for the whole invocation.
start_streaming_activation() {
    local bazel_pid=$1
    stream_pid=""
    if [[ -n "$result_task" || "${resmoke_disable_rbe}" == "true" || "${generate_burn_in_targets}" == "true" ]]; then
        return
    fi
    echo "Starting streaming result task activation..."
    AWS_ACCESS_KEY_ID="${aws_key_new}" AWS_SECRET_ACCESS_KEY="${aws_secret}" \
        python buildscripts/stream_result_task_activation.py \
        --expansion-file ../expansions.yml \
        --build-events-file build_events.json \
        --bazel-pid "$bazel_pid" &
    stream_pid=$!
}

# Build with retries, then test. Leaves the result in the global RET.
run_build_and_test() {
    local build_attempts=3
    local test_attempts=1
    if [[ "${resmoke_disable_rbe}" == "true" ]]; then
        # Local exec runs a full suite serially on a single host, extend the
        # bazel-level timeout well beyond the remote-exec default so the run can finish.
        test_timeout_seconds=14400
        export test_timeout_seconds
    fi

    # Build the test targets before running them, retrying genuine build failures. `bazel
    # build` fetches all external dependencies as a prerequisite of compiling, so this single
    # retrying phase subsumes a separate `bazel fetch` (and the repo cache means a compile
    # failure on a later attempt won't re-download what an earlier attempt already fetched).
    # `bazel test` builds and runs in one command, so without this phase a build failure
    # during the test phase below would not be retried (that phase runs with RETRY_ON_FAIL=0
    # so test failures fail fast and are reported faithfully). The build outputs land in the
    # same output_base, so the `bazel test` below reuses them from cache and only executes the
    # tests. Keep the flags identical to the test invocation (minus the BEP file, which is
    # cache-neutral) so the test phase does not re-analyze and rebuild. The `test` command
    # runs with --build_tests_only (set under the test: config in .bazelrc), which limits the
    # build to test targets and their deps; pass it explicitly here so this `build` builds the
    # same set rather than every target in the ${targets} pattern. Also force
    # --remote_download_outputs=minimal: this phase only needs to confirm the targets compile,
    # so on a remote build it should leave outputs in the CAS rather than download test
    # binaries (and mongod) to this host. Tests execute remotely, so the test phase consumes
    # them straight from the CAS and downloads only what its own policy requires. The default
    # is "all" outside the remote_test config, hence the explicit override; it is a no-op for
    # local exec (no remote executor).
    #
    # The build of some resmoke_suite_test data requires long downloads from repository rules.
    # To avoid destroying the full makespan, defer them so they overlap with test execution. This
    # should still preserve the importance of the retried build for sporadic compiler failures.
    local build_targets
    build_targets="$(targets_for_build_phase)"

    if [[ -n "$build_targets" ]]; then
        export RETRY_ON_FAIL=1
        bazel_evergreen_shutils::retry_bazel_cmd $build_attempts "$BAZEL_BINARY" \
            build --build_tests_only --remote_download_outputs=minimal ${ci_flags} ${bazel_args} ${bazel_compile_flags} ${task_compile_flags} ${patch_compile_flags} -- ${build_targets}
        RET=$?

        if [[ "$RET" != "0" ]]; then
            return
        fi
    else
        echo "All requested targets are deferred to the test phase; skipping the pre-build."
    fi

    export RETRY_ON_FAIL=0
    # Set the timeout for the test phase independently from the build phase above:
    build_timeout_seconds="${test_timeout_seconds:-${build_timeout_seconds:-}}"
    export build_timeout_seconds

    # Drop any BEP left by a previous execution of this task or by another bazel command.
    rm -f build_events.json engflow_links.json "$STREAMED_TASKS_FILE"

    bazel_evergreen_shutils::retry_bazel_cmd $test_attempts "$BAZEL_BINARY" \
        test ${ci_flags} ${bazel_args} ${bazel_compile_flags} ${task_compile_flags} ${patch_compile_flags} --build_event_json_file=build_events.json ${targets} &
    local bazel_bg_pid=$!

    start_streaming_activation "$bazel_bg_pid"

    wait "$bazel_bg_pid"
    RET=$?

    if [[ -n "${stream_pid:-}" ]]; then
        # Let the watcher do its final pass; it exits on its own once the bazel
        # process above is gone.
        wait "$stream_pid" || echo "WARNING: streaming activation watcher exited with an error."
    fi

    if [[ "$RET" -eq 124 ]]; then
        echo "Bazel timed out after ${build_timeout_seconds:-<unspecified>} seconds."
    elif [[ "$RET" != "0" ]]; then
        echo "Errors were found during bazel test, failing the execution"
    fi
}

write_engflow_links() {
    if [[ "${resmoke_disable_rbe}" == "true" ]]; then
        return # Local execution never reaches EngFlow.
    fi
    local invocation_id
    invocation_id=$(engflow_links::invocation_id build_events.json)
    if [[ -z "$invocation_id" ]]; then
        echo "No invocation id in build_events.json; not attaching an EngFlow link."
        return
    fi
    # Result tasks attach their own, per-target link from fetch_remote_test_results.sh.
    engflow_links::entry "EngFlow invocation" "$(engflow_links::invocation_url "$invocation_id")" |
        jq --slurp '.' >engflow_links.json
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
    local extra_args=""
    if [[ "${resmoke_disable_rbe}" != "true" ]]; then
        extra_args="--build-events-file build_events.json"
    fi
    python buildscripts/evergreen_activate_result_tasks.py --expansion-file ../expansions.yml ${extra_args}
}

# The incompatible_with_bazel_remote_test suites run as standalone tasks on the host. Activate them
# early from the runner so they run concurrently with the remote bazel test rather than waiting for
# it to finish. Best-effort: a hiccup here must not abort the runner's remote execution.
activate_local_tasks_early() {
    if [ "${generate_burn_in_targets}" = "true" ]; then
        return
    fi
    echo "Activating standalone local-exec tasks early..."
    python buildscripts/evergreen_activate_result_tasks.py \
        --expansion-file ../expansions.yml --local-only ||
        echo "WARNING: failed to activate local-exec tasks early; continuing with remote execution."
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

    # Runner only (result tasks set $result_task): kick off the standalone local-exec tasks before
    # starting the long remote run, so the two proceed in parallel.
    if [[ -z "$result_task" ]]; then
        activate_local_tasks_early
    fi

    maybe_generate_burn_in_targets

    if [[ "${resmoke_disable_rbe}" == "true" && -z "$result_task" ]]; then
        # Local exec runner: skip bazel entirely; each result task will run its own bazel test.
        activate_result_tasks
        exit 0
    fi

    set +o errexit
    run_build_and_test
    write_engflow_links
    set -o errexit

    if [[ -n "$result_task" ]]; then
        if [[ "${resmoke_disable_rbe}" != "true" ]]; then
            # Reaching here means this result task ran bazel test itself rather than fetching the
            # runner's results. Attach the binaries from this test with debug info to this task's artifacts.
            gather_failed_tests
        fi
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
