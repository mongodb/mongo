#!/bin/bash

set -o errexit
set -o verbose

# Get a random value with leading zeroes removed, /bin/sh version.
rando() {
  tr -cd 0-9 </dev/urandom | head -c 5 | sed -e 's/0*\(.\)/\1/'
}

if [ $# -lt 1 ]; then
    echo "Error: Not enough arguments given."
    echo "Current args: $@"
    exit 1
fi

times=$1
shift
checkpoint_args=$@

if [ "$(basename "$(pwd)")" != "checkpoint" ]; then
    echo "This script must be run from the cmake_build/test/checkpoint folder"
    exit 1;
fi

toolsdir=../../../tools
wtutil=../../wt

r=$(rando)$(rando)
x0=$(rando)$(rando)

# Always run with timestamps and in the predictable mode
base_args="-x -R"

rm -rf RUNDIR_0
# The first run is for calibration only.  We just want to run for the designated
# time and get an approriate stop timestamp that can be used in later runs.
calibration_run_args="-PSD$r,E$x0"
./test_checkpoint -h RUNDIR_0 $base_args $checkpoint_args $calibration_run_args || exit 1
echo "Finished calibration run"
stable_hex=$($toolsdir/wt_timestamps RUNDIR_0 | sed -e '/stable=/!d' -e 's/.*=//')
stop_ts=$(echo $((0x$stable_hex)))

for i in $(seq $times); do
  echo Iteration $i/$times
  x1=$(rando)$(rando)
  x2=$(rando)$(rando)
  rm -rf RUNDIR_1 RUNDIR_2
  # Do two runs up to the stable timestamp, using the same data seed,
  # but with a different extra seed.  Compare it when done.
  first_run_args="-PSD$r,E$x1 -S $stop_ts"
  echo "First run with args $base_args $checkpoint_args $first_run_args"
  ./test_checkpoint -h RUNDIR_1 $base_args $checkpoint_args $first_run_args || exit 1
  second_run_args="-PSD$r,E$x2 -S $stop_ts"
  echo "Second run with args $base_args $checkpoint_args $second_run_args"
  ./test_checkpoint -h RUNDIR_2 $base_args $checkpoint_args $second_run_args || exit 1
  # Compare the runs.
  $toolsdir/wt_cmp_dir RUNDIR_1 RUNDIR_2 || exit 1
done
