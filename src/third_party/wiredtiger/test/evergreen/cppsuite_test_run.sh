#!/bin/bash

set -o verbose

if [[ $# -lt 1 || $# -gt 3 ]]; then
    echo "Error: invalid number of arguments."
    echo "Usage: format_test_predictable.sh ${test_name} ${test_config_filename} ${test_config}"
    echo "Current args: $@"
    exit 1
fi

test_name=$1
test_config_filename=$2
test_config=$3

# Get the current setting of the required perf field
paranoia_level=`sudo sysctl kernel.perf_event_paranoid | cut -d '=' -f 2 | xargs`
sudo sysctl -w kernel.perf_event_paranoid=2
./run -t "$test_name" -C "$test_config" -f "$test_config_filename" -l 2
exit_code=$?
# Restore the kernel perf event paranoid level
sudo sysctl -w kernel.perf_event_paranoid=$paranoia_level
echo "$exit_code" > cppsuite_exit_code
if [ "$exit_code" != 0 ]; then
  echo "[{\"info\":{\"test_name\": \"$test_name\"},\"metrics\": []}]" > "$test_name".json
fi
exit 0
