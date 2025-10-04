#!/bin/bash

set -o errexit
set -o verbose

# Get a random value with leading zeroes removed, /bin/sh version.
rando() {
  tr -cd 0-9 </dev/urandom | head -c 5 | sed -e 's/0*\(.\)/\1/'
}

if [ $# -ne 1 ]; then
    echo "Error: invalid number of arguments."
    echo "Current args: $@"
    exit 1
fi

times=$1

runtime=20  # seconds
nthreads=5

if [ "$(basename "$(pwd)")" != "schema_abort" ]; then
    echo "This script must be run from the build/test/csuite/schema_abort folder"
    exit 1;
fi

toolsdir=../../../../tools
wtutil=../../../wt

r=$(rando)$(rando)
x0=$(rando)$(rando)

rm -rf RUNDIR_0
# The first run is for calibration only.  We just want to run for the designated
# time and get an appropriate stop timestamp that can be used in later runs.
calibration_run_args="-PSD$r,E$x0 -T $nthreads -t $runtime"
./test_schema_abort -p -h RUNDIR_0 $calibration_run_args || exit 1
echo "Finished calibration run"
stable_hex=$($toolsdir/wt_timestamps RUNDIR_0/WT_HOME | sed -e '/stable=/!d' -e 's/.*=//')
op_count=$(echo $((0x$stable_hex)))

for i in $(seq $times); do
  echo Iteration $i/$times
  x1=$(rando)$(rando)
  x2=$(rando)$(rando)
  rm -rf RUNDIR_1 RUNDIR_2

  # Run with up to a slightly different timestamp for each iteration.
  ops=$(($op_count + $(rando) % 100))

  # Do two runs up to the stable timestamp, using the same data seed,
  # but with a different extra seed.  Compare it when done.
  first_run_args="-PSD$r,E$x1 -T $nthreads -s $ops"
  echo "First run with args $first_run_args"
  ./test_schema_abort -p -h RUNDIR_1 $first_run_args  || exit 1

  second_run_args="-PSD$r,E$x2 -T $nthreads -s $ops"
  echo "Second run with args $second_run_args"
  ./test_schema_abort -p -h RUNDIR_2 $second_run_args  || exit 1

  # We are ignoring the table:wt table. This table does not participate in
  # predictable replay, as it may be concurrently created, opened (regular or bulk cursor),
  # verified and dropped by multiple threads in test_schema_abort.
  $toolsdir/wt_cmp_dir -i '^table:wt$' RUNDIR_1/WT_HOME RUNDIR_2/WT_HOME || exit 1
done
