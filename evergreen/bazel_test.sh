# Usage:
#   bazel_test [arguments]
#
# Required environment variables:
# * ${targets} - Test targets
# * ${bazel_args} - Extra command line args to pass to "bazel test"

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose
set -o pipefail

. "$DIR/bazel_evergreen_shutils.sh"

bazel_evergreen_shutils::activate_and_cd_src
bazel_evergreen_shutils::export_ssl_paths_if_needed

# Windows tests execute locally on the host and the windows test runner does not
# materialize data deps into the runfiles tree when the runfiles paths exceed the 260
# character MAX_PATH limit (the golden data files of the deep test directories), so
# golden tests could not find their data files. Point them at the checked-out source
# tree instead: its paths stay well under the limit. Convert with cygpath -m: the shell
# is cygwin and pwd is a /cygdrive/... path that boost::filesystem cannot use.
if bazel_evergreen_shutils::is_windows; then
    export GOLDEN_TEST_DATA_ROOT="$(cygpath -m "$(pwd)")"
    # A bare --test_env=GOLDEN_TEST_DATA_ROOT does not deliver the client environment
    # variable to locally executed test actions on Windows (the test falls back to the
    # runfiles lookup, whose data deps tw.cc does not materialize past MAX_PATH), so
    # pass the value explicitly. Appended last, it wins over the task's bare flag.
    # Shared unit-test tasks explicitly select the Linux test_wrapper, so the
    # Windows launcher must also be supplied last to override that task flag.
    task_compile_flags="${task_compile_flags:-} --test_env=GOLDEN_TEST_DATA_ROOT=$GOLDEN_TEST_DATA_ROOT --run_under=//bazel:windows_test_launcher"
fi

# Use `eval` to force evaluation of the environment variables in the echo statement:
eval echo "Execution environment: Targets: ${targets}"

BAZEL_BINARY="$(bazel_evergreen_shutils::bazel_get_binary_path)"
export BAZEL_BINARY

# Mode-specific LOCAL_ARG and release flag
LOCAL_ARG="$(bazel_evergreen_shutils::compute_local_arg test)"

# If we are doing a patch build or we are building a non-push
# build on the waterfall, then we don't need the --release
# flag. Otherwise, this is potentially a build that "leaves
# the building", so we do want that flag.
LOCAL_ARG="$(bazel_evergreen_shutils::maybe_release_flag "$LOCAL_ARG")"

# Possibly scale test timeout and append to bazel_args
bazel_evergreen_shutils::maybe_scale_test_timeout_and_append

MONGO_VERSION_ARG="--define=MONGO_VERSION=${version}"
if [[ -n "${no_mongo_version}" ]]; then
    MONGO_VERSION_ARG=""
fi

# Build the shared flags and persist the --config subset
ALL_FLAGS="--verbose_failures ${LOCAL_ARG} ${MONGO_VERSION_ARG} ${bazel_args:-} ${bazel_compile_flags:-} ${task_compile_flags:-} ${patch_compile_flags:-}"
echo "${ALL_FLAGS}" >.bazel_build_flags

SPLIT_WINDOWS_TESTS=false
BUILD_BEP_FLAG=""
if bazel_evergreen_shutils::is_windows && [[ "${task_name:-}" == unit_tests ]]; then
    SPLIT_WINDOWS_TESTS=true
    BUILD_BEP_FLAG="--build_event_json_file=build_events.build.json"
fi

# to capture exit codes
set +o errexit

# Build then test with retries. `bazel build` fetches all external dependencies as a
# prerequisite of compiling, so this retrying phase subsumes a separate `bazel fetch` and also
# retries genuine build failures. `bazel test` builds and runs in one command, so without a
# dedicated build phase a build failure would slip into the test phase below, which runs with
# RETRY_ON_FAIL=0 (so test failures fail fast and are reported faithfully) and would not be
# retried. The build outputs land in the same output_base, so `bazel test` reuses them from
# cache and only executes the tests. Keep the flags identical (minus the cache-neutral BEP
# file) so the test phase does not re-analyze and rebuild. The `test` command runs with
# --build_tests_only (set under the test: config in .bazelrc), which limits the build to test
# targets and their deps; pass it explicitly here so this `build` builds the same set rather
# than every target in the ${targets} pattern (e.g. //src/...). Also force
# --remote_download_outputs=minimal: this phase only needs to confirm the targets compile, so
# on a remote build it should leave outputs in the CAS rather than download test binaries to
# this host. Tests execute remotely, so the test phase consumes them straight from the CAS and
# downloads only what its own policy requires. The default is "all" outside the remote_test
# config, hence the explicit override; it is a no-op for local exec (no remote executor).
export RETRY_ON_FAIL=1
bazel_evergreen_shutils::retry_bazel_cmd 3 "$BAZEL_BINARY" \
    build --build_tests_only --remote_download_outputs=minimal ${ALL_FLAGS} ${BUILD_BEP_FLAG} ${targets}
RET=$?

if [[ "$RET" == "0" ]]; then
    export RETRY_ON_FAIL=0
    if $SPLIT_WINDOWS_TESTS; then
        # Use the build's selected tests: adding a positive tag to test_tag_filters
        # would OR it with the task's existing tags and could widen test coverage.
        "$python" buildscripts/bazel_test_groups.py build_events.build.json \
            bazel-test-targets.normal.txt bazel-test-targets.timing_sensitive.txt
        RET=$?
    fi

    if [[ "$RET" == 0 ]]; then
        if $SPLIT_WINDOWS_TESTS && [[ -s bazel-test-targets.timing_sensitive.txt ]]; then
            : >build_events.json
            for group in normal timing_sensitive; do
                if [[ ! -s "bazel-test-targets.${group}.txt" ]]; then
                    continue
                fi
                GROUP_FLAGS=()
                if [[ "$group" == timing_sensitive ]]; then
                    GROUP_FLAGS=(--local_test_jobs=1)
                fi
                echo "Running Windows ${group} unit tests."
                : >"build_events.${group}.json"
                bazel_evergreen_shutils::retry_bazel_cmd 3 "$BAZEL_BINARY" \
                    test ${ALL_FLAGS} "${GROUP_FLAGS[@]}" \
                    --build_event_json_file="build_events.${group}.json" \
                    --target_pattern_file="bazel-test-targets.${group}.txt"
                GROUP_RET=$?
                # Both passes run even if the first fails. Retain their BEP records
                # for failed-binary gathering, and preserve the first failure status.
                if [[ -s "build_events.${group}.json" ]]; then
                    cat "build_events.${group}.json" >>build_events.json || GROUP_RET=$?
                elif [[ "$GROUP_RET" == 0 ]]; then
                    GROUP_RET=1
                fi
                if [[ "$RET" == 0 ]]; then
                    RET="$GROUP_RET"
                fi
            done
        else
            bazel_evergreen_shutils::retry_bazel_cmd 3 "$BAZEL_BINARY" \
                test ${ALL_FLAGS} --build_event_json_file=build_events.json ${targets}
            RET=$?
        fi
    fi

    if [[ "$RET" -eq 124 ]]; then
        echo "Bazel timed out after ${build_timeout_seconds:-<unspecified>} seconds."
    elif [[ "$RET" != "0" ]]; then
        echo "Errors were found during bazel test, failing the execution"
    fi
fi

bazel_evergreen_shutils::write_last_engflow_link

set -o errexit

if [[ "$RET" != "0" ]]; then
    bazel_evergreen_shutils::gather_failed_tests || true
fi

: "${RET:=1}"
exit "${RET}"
