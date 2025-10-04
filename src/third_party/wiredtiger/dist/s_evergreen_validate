#!/bin/bash

. `dirname -- ${BASH_SOURCE[0]}`/common_functions.sh
setup_trap
cd_top
check_fast_mode_flag

fast=false
exit1=0
exit2=0
if [[ -n "$1" && "$1" != "-F" ]]; then
    echo "Usage: $0 [-F]"
    echo "-F only run validation commands if evergreen yml files have been modified"
    exit 1
fi

if [ $(command -v evergreen) ] && [ -f ~/.evergreen.yml ]; then
    if is_fast_mode ; then
        # Check the evergreen.yml files for modifications.
        search=`git diff --name-only "$(last_commit_from_dev)" | grep -E 'evergreen.*\.yml$'`
        # If we didn't find any files then exit.
        if test -z "$search"; then
            exit 0
        fi
    fi
    echo "Validating evergreen.yml " > dist/$t
    evergreen validate -p wiredtiger test/evergreen.yml >> dist/$t 2>&1
    exit1=$?
    echo "=-=-=-=-=-=-=-=-=-=-=" >> dist/$t
    echo "Validating evergreen_develop.yml" >> dist/$t
    evergreen validate -p wiredtiger test/evergreen_develop.yml >> dist/$t 2>&1
    exit2=$?
    cd dist
fi

if [ "$exit1" -ne 0 -o "$exit2" -ne 0 ]; then
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
    echo "$0 failed with output:"
    cat $t
    echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
fi
exit 0
