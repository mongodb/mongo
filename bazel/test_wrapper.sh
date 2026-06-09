#!/bin/bash

# Golden tests are currently incompatible with Bazel Remote Execution
unset GOLDEN_TEST_CONFIG_PATH
unset GOLDEN_TEST_OUTPUT_ROOT_PATTERN

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

# Run the test in its own process group so that, on timeout, we can signal both the
# test process and any child processes it has spawned (e.g. per-test forked workers
# used for isolation). Without setsid the test binary is in the shell's process group
# and kill can only reach the parent, leaving child cores uncaptured (DEVPROD-27556).
# setsid is Linux-only (util-linux); on macOS we fall back to a plain exec and accept
# that child processes won't be cored on timeout.
if command -v setsid &>/dev/null; then
    setsid "${test_bin}" "${@:2}" &
    main_pid=$!
    main_pgid=$main_pid # setsid makes the test the process group leader
    echo "Process-under-test started with PID: ${main_pid} (process group: ${main_pgid})"
else
    "${test_bin}" "${@:2}" &
    main_pid=$!
    main_pgid=""
    echo "Process-under-test started with PID: ${main_pid} (setsid unavailable; child processes will not be cored on timeout)"
fi

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
        if [[ -n "$main_pgid" ]]; then
            echo "${timeout_minutes} minute Timer finished. Process-under-test ${main_pid} is still running. Sending SIGABRT to process group ${main_pgid} to capture coredumps from parent and child processes."
            # Use a negative PGID to signal the entire process group so child processes
            # (e.g. per-test workers forked for isolation) are also cored (DEVPROD-27556).
            kill -ABRT -"${main_pgid}"
        else
            echo "${timeout_minutes} minute Timer finished. Process-under-test ${main_pid} is still running. Sending SIGABRT to trigger a coredump now."
            kill -ABRT "${main_pid}"
        fi
    fi
) &

wait "${main_pid}"
RET=$?

# Wait for every process in the test's process group to exit before scanning for cores.
# The parent (main_pid) is gone, but children signaled at the same time may still be
# writing their core files. Poll instead of sleeping a fixed amount so we wait exactly
# as long as necessary. Cap at 30 s in case of unkillable stragglers.
if [[ -n "$main_pgid" ]]; then
    _core_wait_end=$((SECONDS + 30))
    while kill -0 -"${main_pgid}" 2>/dev/null && ((SECONDS < _core_wait_end)); do
        sleep 0.1
    done

    # Collect all core files; both the parent and any forked child processes may each produce one.
    while IFS= read -r CORE_FILE; do
        [ -f "$CORE_FILE" ] || continue
        CORE_FILENAME="dump_$(date +%s%N).core.gz"
        gzip -c "$CORE_FILE" >"$TEST_UNDECLARED_OUTPUTS_DIR/$CORE_FILENAME"
        echo "Writing coredump to $CORE_FILENAME..."
    done < <(find -L ./ -name "*.core")
fi

# Collect all core files; both the parent and any forked child processes may each produce one.
while IFS= read -r CORE_FILE; do
    [ -f "$CORE_FILE" ] || continue
    CORE_FILENAME="dump_$(date +%s)_$$.core.gz"
    gzip -c "$CORE_FILE" >"$TEST_UNDECLARED_OUTPUTS_DIR/$CORE_FILENAME"
    echo "Writing coredump to $CORE_FILENAME..."
done < <(find -L ./ -name "*.core")

exit $RET
