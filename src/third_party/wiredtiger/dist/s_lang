#!/bin/bash

# Check lang directories for potential name conflicts
. `dirname -- ${BASH_SOURCE[0]}`/common_functions.sh
setup_trap
cd_top
cd lang

for d in *; do
    f=`find $d -name 'wiredtiger_wrap.c'`
    test -z "$f" && continue

    sed -e '/SWIGINTERN.*__wt_[a-z][a-z]*_[a-z]/!d' \
        -e '/__wt_[^(]*__.*(/d' \
        -e '/_wrap/d' \
        -e "/_${d}_/d" \
        $f > $t

    test -s $t && {
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
        echo "$l: potential SWIG naming conflict"
        echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
        cat $t
        exit 1
    }
done

exit 0
