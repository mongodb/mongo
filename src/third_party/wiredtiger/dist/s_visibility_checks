#!/bin/bash

. `dirname -- ${BASH_SOURCE[0]}`/common_functions.sh
setup_trap
cd_top

# Check to see if a time aggregate is used in a straight visibility check.
# It should never be - the "snap_min" visibility check should be used instead.
found=`grep -rnI "txn_visible(.*addr.ta" src/`
if [ ! -z "$found" ]; then
	echo "Found invalid usage(s) of visibility check and a time aggregate window"
	echo "$found"
	exit 1
fi

exit 0
