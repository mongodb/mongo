#!/bin/bash

set -o errexit
set -o verbose

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Error: invalid number of arguments."
    echo "Usage: format_test_predictable.sh ${times} ${test_format_extra_args}"
    echo "Current args: $@"
    exit 1
fi

times=$1
test_format_extra_args=$2

# Get a random value with leading zeroes removed, /bin/sh version.
rando() {
  tr -cd 0-9 </dev/urandom | head -c 5 | sed -e 's/0*\(.\)/\1/'
}

# Fail, showing the configuration file.
fail() {
  echo "======= FAILURE =========="
  for file; do
    if [ -f "$file" ]; then
      echo Contents of "$file":
      cat "$file"
      echo "================"
    fi
  done
  exit 1
}
runtime=3  # minutes
config=../../../test/format/CONFIG.replay
for i in $(seq $times); do
  echo Iteration $i/$times
  x2=$(rando)
  x3=$(rando)
  rm -rf RUNDIR_1 RUNDIR_2 RUNDIR_3

  first_run_args="-c $config runs.timer=$runtime"
  ./t -h RUNDIR_1 $first_run_args $test_format_extra_args || fail RUNDIR_1/CONFIG 2>&1
  stable_hex=$(../../../tools/wt_timestamps RUNDIR_1 | sed -e '/stable=/!d' -e 's/.*=//')
  ops=$(echo $((0x$stable_hex)))

  # Do the second run up to the stable timestamp, using the same data seed,
  # but with a different extra seed.  Compare it when done.
  common_args="-c RUNDIR_1/CONFIG runs.timer=0 runs.ops=$ops"
  ./t -h RUNDIR_2 $common_args random.extra_seed=$x2 || fail RUNDIR_2/CONFIG 2>&1
  ../../../tools/wt_cmp_dir RUNDIR_1 RUNDIR_2 || fail RUNDIR_1/CONFIG RUNDIR_2/CONFIG 2>&1

  # Do the third run up to the stable timestamp, using the same data seed,
  # but with a different extra seed.  Compare it to the second run when done.
  ./t -h RUNDIR_3 $common_args random.extra_seed=$x3 || fail RUNDIR_3/CONFIG 2>&1
  ../../../tools/wt_cmp_dir RUNDIR_2 RUNDIR_3 || fail RUNDIR_2/CONFIG RUNDIR_3/CONFIG 2>&1
done
