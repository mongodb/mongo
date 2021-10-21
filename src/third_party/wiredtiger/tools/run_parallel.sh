#!/usr/bin/env bash

usage () {
    cat << EOF
Usage: run_parallel.sh {command} {num_iter} {num_parallel}
Where:
  {command} is a string containing the command, including parameters, to run
  {num_iter} is an positive integer indicating how many iterations to execute
  [num_parallel] is an (optional) positive integer indicating how many parallel commands should be executed
     in each iteration. If not provided, the default is half the number of available CPU cores.
EOF
}

if [ "$#" -lt  2 ]; then
    echo "Illegal number of parameters."
    usage
    exit 1
fi

# Determine the number of CPU cores. This code is Linux specific at the moment.
NCORES=$(grep -c ^processor /proc/cpuinfo)

command=$1
num_iter=$2

if [ "$#" -eq  3 ]; then
  num_parallel=$3
else
  # Use half the number of processor cores
  num_parallel="$(($NCORES / 2))"
fi

echo "run_parallel:"
echo "  number of cores: $NCORES"
echo "  command:         $command"
echo "  num_parallel:    $num_parallel"
echo "  num_iter:        $num_iter"

outf=./outfile.txt

for i in $(seq $num_iter); do
  echo "Starting iteration $i" >> $outf
  echo "Starting iteration $i"

  process_ids=()
  # start the commands in parallel
  for((t=1; t<=num_parallel; t++)); do
    echo "Starting parallel command $t (of $num_parallel) in iteration $i (of $num_iter)" >> nohup.out.$t
    eval nohup $command >> nohup.out.$t 2>&1 &
    process_ids[$t]=$!
  done

  # Wait for the commands to all complete
  for((t=1; t<=num_parallel; t++)); do
    wait ${process_ids[$t]}
    err=$?
    if [[ $err -ne 0 ]]
    then
      echo "iteration $i of parallel command $t failed with $err error code"
      exit $err
    fi
  done
done
