#!/bin/bash

set -o errexit
set -o verbose

if [ $# -ne 4 ]; then
    echo "Error: invalid number of arguments."
    echo "Usage: format_test_predictable.sh ${times} ${no_of_procs} ${wt_config} ${timestamp_config}"
    echo "Current args: $@"
    exit 1
fi

times=$1
no_of_procs=$2
wt_config=$3
timestamp_config=$4

export WIREDTIGER_CONFIG='checkpoint_sync=0,transaction_sync=(method=none)'

CMD='./test_checkpoint -h WT_TEST.$i.$t ${timestamp_config} -t r -r 2 -W 3 -n 1000000 -k 1000000 -C ${wt_config}'

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
