#!/bin/bash

. `dirname -- ${BASH_SOURCE[0]}`/common_functions.sh
setup_trap
cd_dist
check_fast_mode_flag

# Complain if someone uses the wrong getopt.
find ../src ../test ../bench -name '*.c' | filter_if_fast ../ | xargs grep -E '[^a-z_]getopt\(' > $t

test -s $t && {
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo 'Calls to the C library version of getopt.'
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    cat $t
    exit 1
}
exit 0
