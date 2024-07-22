#!/bin/bash

. `dirname -- ${BASH_SOURCE[0]}`/common_functions.sh
setup_trap
cd_top

error=0

unspecified_dependency=false

pip_installs=$(grep -E 'pip3? install' "test/evergreen.yml")

while IFS= read -r line; do
    for word in $line; do
        if [[ "$word" =~ ^pip3?$ || "$word" == "install" || "$word" =~ ^python3?$ || "$word" =~ -.* ]]; then
            continue
        fi
        # pip uses == to specify a specific version. Make sure these characters are present
        if [[ ! "$word" =~ .*==.* ]]; then
            unspecified_dependency=true
            echo "Version not specified for Python dependency $word in line: $line"
        fi
    done
done <<< "$pip_installs"

if $unspecified_dependency; then
    error=1
fi

program=test/evergreen/evg_cfg.py

# Run checking program to identify missing tests in Evergreen configuration
${program} check >$t 2>&1
e=$?

if [[ $e != 0 ]]; then
    error=1
fi

test -s $t && {
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "$0: $program check"
    cat $t
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

    # Don't exit non-zero unless the python script did, the script
    # requires python modules that are commonly not installed, and
    # in that case it exits 0. Post the complaint, but don't fail.
    exit $e
}
exit $error
