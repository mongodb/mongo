#!/bin/bash

ulimit -c unlimited

eval ${@:1} &
main_pid=$!
echo "Process-under-test started with PID: ${main_pid}"

(
    sleep 600

    # 'kill -0' checks for process existence without sending a signal
    if kill -0 "$main_pid" 2>/dev/null; then
        echo "10 minute Timer finished. Process-under-test ${main_pid} is still running. Sending a SIGABRT to trigger a coredump now."
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
