#!/usr/bin/env bash

set -euf -o pipefail
. `dirname -- ${BASH_SOURCE[0]}`/common_functions.sh
cd_top
check_fast_mode_flag

# All test scripts should be routed through "run.py", running them directly is an anti-pattern.
# Use the executable bits on the scripts as weak evidence of the anti-pattern.
exes=$(find test/suite -type f \( -perm -u+x -o -perm -g+x -o -perm -o+x \) ! -name 'run.py' | filter_if_fast)
if [[ -n "$exes" ]]; then
    echo "The following files should not be executable:"
    echo "$exes"
    exit 1
fi

exit 0
