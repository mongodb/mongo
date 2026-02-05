#!/bin/bash

# Golden tests are currently incompatible with Bazel Remote Execution
unset GOLDEN_TEST_CONFIG_PATH

is_ppc64le() {
    local -r arch="$(uname -m)"
    [[ "${arch}" == "ppc64le" || "${arch}" == "ppc64" || "${arch}" == "ppc" ]] && return 0 || return 1
}

is_s390x() {
    local -r arch="$(uname -m)"
    [[ "${arch}" == "s390x" || "${arch}" == "s390" ]] && return 0 || return 1
}

is_s390x_or_ppc64le() {
    if is_ppc64le || is_s390x; then
        return 0
    else
        return 1
    fi
}

ulimit -c unlimited

# If RUNFILES_DIR is set, prepend "_main/" to the path to find the binary in the runfiles tree.
# With --nolegacy_external_runfiles, binaries are located under $RUNFILES_DIR/_main/...
test_bin="$1"
if [[ -n "${RUNFILES_DIR:-}" && ! -x "$test_bin" ]]; then
    test_bin="${RUNFILES_DIR}/_main/${test_bin}"
fi

"${test_bin}" "${@:2}" &
main_pid=$!
echo "Process-under-test started with PID: ${main_pid}"

# This is mocked out in buildscripts/bazel_testbuilds/verify_unittest_coredump_test.sh, make sure
# to update the test if this is changed.
timeout_seconds=600

if is_s390x_or_ppc64le; then
    timeout_seconds=$((timeout_seconds * 4))
fi

timeout_minutes=$((timeout_seconds / 60))

(
    sleep $timeout_seconds

    # 'kill -0' checks for process existence without sending a signal
    if kill -0 "$main_pid" 2>/dev/null; then
        echo "${timeout_minutes} minute Timer finished. Process-under-test ${main_pid} is still running. Sending a SIGABRT to trigger a coredump now."
        kill -ABRT "${main_pid}"
    fi
) &

wait "${main_pid}"
RET=$?

CORE_FILE=$(find -L ./ -name "*.core")
if [ -f "$CORE_FILE" ]; then
    CORE_FILENAME="dump_$(date +%s%N).core.gz"
    gzip -c $CORE_FILE >"$TEST_UNDECLARED_OUTPUTS_DIR/$CORE_FILENAME"
    echo "Writing coredump to $CORE_FILENAME..."
fi

exit $RET
