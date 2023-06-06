#!/bin/sh

export LD_LIBRARY_PATH=/opt/bin:/opt/tools/voidstar/lib:$LD_LIBRARY_PATH
cd ./bin/test/format
./t "$@" 2>&1 &
T_PID=$!
wait $T_PID
RET=$?
if [[ $RET -ne 0 ]]; then
    gdb --eval-command="thread apply all backtrace 30" --eval-command="quit" --batch t *core*
fi
exit $RET
