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

set -o errexit
set -o verbose

bazel_evergreen_shutils::activate_and_cd_src

BAZEL_BINARY=$(bazel_evergreen_shutils::bazel_get_binary_path)

ci_flags="--//bazel/resmoke:in_evergreen"

# For simple build ID generation:
export compile_variant="${compile_variant}"
export version_id="${version_id}"

if [[ "${evergreen_remote_exec}" == "on" ]]; then
    ci_flags="--config=remote_test ${ci_flags}"
fi

if [ ${should_shuffle} = true ]; then
    ci_flags+=" --test_arg=--shuffle"
elif [ ${should_shuffle} = false ]; then
    ci_flags+=" --test_arg=--shuffleMode=off"
fi

if [ "${is_patch}" = "true" ]; then
    ci_flags+=" --test_arg=--patchBuild"
fi

if [ "${skip_symbolization}" = "true" ]; then
    ci_flags+=" --test_arg=--skipSymbolization"
fi

# Add test selection flag based on patch parameter
if [ "${enable_evergreen_api_test_selection}" = "true" ]; then
    ci_flags+=" --test_arg=--enableEvergreenApiTestSelection"
fi

# Split comma separated list of strategies
IFS=',' read -a strategies <<<"$test_selection_strategies_array"
for strategy in "${strategies[@]}"; do
    ci_flags+=" --test_arg=--evergreenTestSelectionStrategy=${strategy}"
done

ALL_FLAGS="${ci_flags} ${LOCAL_ARG} ${bazel_args:-} ${bazel_compile_flags:-} ${task_compile_flags:-} ${patch_compile_flags:-}"
echo "${ALL_FLAGS}" >.bazel_build_flags

# Save the invocation, intentionally excluding CI specific flags.
echo "python buildscripts/install_bazel.py" >bazel-invocation.txt
echo "bazel test ${bazel_args} ${targets}" >>bazel-invocation.txt

set +o errexit

# Fetch then test with retries.
export RETRY_ON_FAIL=1
bazel_evergreen_shutils::retry_bazel_cmd 3 "$BAZEL_BINARY" \
    fetch ${ci_flags} ${bazel_args} ${bazel_compile_flags} ${task_compile_flags} ${patch_compile_flags} ${targets}
RET=$?

if [[ "$RET" == "0" ]]; then
    export RETRY_ON_FAIL=0
    bazel_evergreen_shutils::retry_bazel_cmd 3 "$BAZEL_BINARY" \
        test ${ci_flags} ${bazel_args} ${bazel_compile_flags} ${task_compile_flags} ${patch_compile_flags} --build_event_json_file=build_events.json ${targets}
    RET=$?

    if [[ "$RET" -eq 124 ]]; then
        echo "Bazel timed out after ${build_timeout_seconds:-<unspecified>} seconds."
    elif [[ "$RET" != "0" ]]; then
        echo "Errors were found during bazel test, failing the execution"
    fi
fi

bazel_evergreen_shutils::write_last_engflow_link

set -o errexit

# Symlink test logs to where Evergreen expects them. Evergreen won't read into a symlinked directory,
# so symlink each log file individually.
find bazel-testlogs/ -type f -path "*TestLogs/**/*.log" -print0 |
    while IFS= read -r -d '' test_outputs; do
        source=${workdir}/src/$test_outputs
        target=${workdir}/build/TestLogs/$(echo $test_outputs | sed -e 's/bazel-testlogs\///g' -e 's/test\.outputs\/build\/TestLogs\///g')
        mkdir -p $(dirname $target)
        ln -sf $source $target
    done

# Symlink data directories to where Resmoke normally puts them for compatibility with post tasks
# that run for all Resmoke tasks.
find bazel-testlogs/ -path '*data/job*' -name 'job*' -print0 |
    while IFS= read -r -d '' test_outputs; do
        source=${workdir}/src/$test_outputs
        target=${workdir}/$(sed 's/.*\.outputs\///' <<<$test_outputs)
        mkdir -p $(dirname $target)
        ln -sf $source $target
    done

# Symlinks archived data directories from multiple tests/shards to a single folder. Evergreen needs a
# single folder it can glob for s3.put. See the Evergreen function "upload mongodatafiles".
target_from_undeclared_outputs() {
    echo ${1} | sed -e 's/^.*bazel-testlogs\/\(.*\)\/test.outputs.*$/\1/'
}
find bazel-testlogs/ -path '*data_archives/*.tgz' -print0 |
    while IFS= read -r -d '' archive; do
        source=${workdir}/src/$archive
        bazel_target_prefix=$(target_from_undeclared_outputs $archive | sed 's/\//_/g')
        target=${workdir}/$(echo $archive | sed -e 's/.*\.outputs\///' -e "s/data_archives\//&$bazel_target_prefix-/g")
        mkdir -p $(dirname $target)
        ln -sf $source $target
    done

# Symlinks test.log from multiple tests/shards to a single folder. Evergreen needs a
# single folder it can glob for s3.put. See the Evergreen function "upload bazel test logs".
find bazel-testlogs/ -path '*test.log' -print0 |
    while IFS= read -r -d '' log; do
        log_renamed=$(echo $log | sed -e 's/bazel-testlogs\///g' -e 's/\//_/g')
        source=${workdir}/src/$log
        target=${workdir}/tmp/bazel-testlogs/$log_renamed
        mkdir -p $(dirname $target)
        ln -sf $source $target
    done
echo "format: text-timestamp" >${workdir}/build/TestLogs/log_spec.yaml
echo "version: 0" >>${workdir}/build/TestLogs/log_spec.yaml

# Combine reports from potentially multiple tests/shards.
find bazel-testlogs/ -name report*.json | xargs $python buildscripts/combine_reports.py --no-report-exit --add-bazel-target-info -o report.json || true

if [[ "$RET" != "0" ]]; then
    # This is a hacky way to save build time for the initial build during the `bazel test` above. They
    # are stripped binaries there. We should rebuild them with debug symbols and separate debug.
    # The relinked binaries should still be hash identical when stripped with strip.
    sed -i -e 's/--config=remote_test//g' -e 's/--separate_debug=False/--separate_debug=True/g' -e 's/--features=strip_debug//g' .bazel_build_flags

    # The --config flag needs to stay consistent for the `bazel run` to avoid evicting the previous results.
    # Strip out anything that isn't a --config flag that could interfere with the run command.
    CONFIG_FLAGS="$(bazel_evergreen_shutils::extract_config_flags "${ALL_FLAGS}")"
    eval ${BAZEL_BINARY} run ${CONFIG_FLAGS} //buildscripts:gather_failed_tests || true
fi

exit $RET
