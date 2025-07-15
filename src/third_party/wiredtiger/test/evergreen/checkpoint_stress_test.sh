#!/bin/bash

set -o errexit
set -o verbose

if [ $# -ne 5 ]; then
    echo "Error: invalid number of arguments."
    echo "Usage: format_test_predictable.sh ${tiered} ${times} ${no_of_procs} ${wt_config} ${timestamp_config}"
    echo "Current args: $@"
    exit 1
fi

tiered=$1
times=$2
no_of_procs=$3
wt_config=$4
timestamp_config=$5

export WIREDTIGER_CONFIG='checkpoint_sync=0,transaction_sync=(method=none)'

CMD='./test_checkpoint -h WT_TEST.$i.$t ${timestamp_config} -t r -r 2 -W 3 -n 1000000 -k 1000000 -C ${wt_config}'

if [ $tiered -eq 1 ]; then
    CMD="$CMD -PT"
fi

for i in $(seq $times); do
  for t in $(seq $no_of_procs); do
    eval nohup $CMD > nohup.out.$i.$t 2>&1 &
  done

  failure=0
  for t in $(seq $no_of_procs); do
    ret=0
    wait -n || ret=$?
    if [ $ret -ne 0 ]; then
      # Skip the below lines from nohup output file because they are very verbose and
      # print only the errors to evergreen log file.
      grep -v "Finished verifying" nohup.out.* | grep -v "Finished a checkpoint" | grep -v "thread starting"
      failure=1
      fail_ret=$ret
    fi
  done
  if [ $failure -eq 1 ]; then
    exit $fail_ret
  fi
done
