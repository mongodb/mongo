#!/bin/bash

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

eval ${@:1} &
main_pid=$!
echo "Process-under-test started with PID: ${main_pid}"

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
    CORE_FILENAME="dump_$(date +%s%N).core"
    mv $CORE_FILE "$TEST_UNDECLARED_OUTPUTS_DIR/$CORE_FILENAME"
    echo "Writing coredump to $CORE_FILENAME..."
fi

exit $RET
