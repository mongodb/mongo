#!/bin/bash

set -o errexit
set -o verbose

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Error: invalid number of arguments."
    echo "Usage: format_test_predictable.sh ${times} ${truncated_log_args}"
    echo "Current args: $@"
    exit 1
fi

times=$1
truncated_log_args=$2

for i in $(seq $times); do
  # Run the various combinations of args. Let time and threads be random. Add a
  # timing stress to test_timestamp_abort every other run.
  if [ $(( $i % 2 )) -eq 0 ]; then
    test_timestamp_abort_args=-s
  else
    test_timestamp_abort_args=
  fi

  # Run current version with write-no-sync txns.
  ./random_abort/test_random_abort 2>&1
  ./timestamp_abort/test_timestamp_abort $test_timestamp_abort_args 2>&1

  # Current version with memory-based txns (MongoDB usage).
  ./random_abort/test_random_abort -m 2>&1
  ./timestamp_abort/test_timestamp_abort -m $test_timestamp_abort_args 2>&1

  # V1 log compatibility mode with write-no-sync txns.
  ./random_abort/test_random_abort -C 2>&1
  ./timestamp_abort/test_timestamp_abort -C $test_timestamp_abort_args 2>&1

  # V1 log compatibility mode with memory-based txns.
  ./random_abort/test_random_abort -C -m 2>&1
  ./timestamp_abort/test_timestamp_abort -C -m $test_timestamp_abort_args 2>&1

  ./truncated_log/test_truncated_log $truncated_log_args 2>&1

  # Just let the system take a breath
  sleep 10s
done
