#!/bin/bash

ulimit -c unlimited

$1
RET=$?

CORE_FILE=$(find -L ./ -name "core")
if [ -f "$CORE_FILE" ]; then
    CORE_FILENAME="dump_$(date +%s%N).core"
    mv $CORE_FILE "$TEST_UNDECLARED_OUTPUTS_DIR/$CORE_FILENAME"
    echo "Writing coredump to $CORE_FILENAME..."
fi

exit $RET
