#!/bin/bash

# Complain about unused #defines.
. `dirname -- ${BASH_SOURCE[0]}`/common_functions.sh
cd_dist
use_pygrep
check_fast_mode_flag

# List of source files to search.
l=$(
    sed -e '/^[a-z]/!d' -e 's/[	 ].*$//' -e 's,^,../,' filelist
    ls -1 ../src/*/*_inline.h ../src/utilities/*.c ../test/*/*.c
)

# List of include files for source #defines.
# Ignore the queue.h file, we don't use most of it.
dl=$(ls -1 ../src/*/*.h ../src/include/*.in | grep -F -v queue.h)

# If no source files or include files have changed there's nothing to do. Early exit the script
[[ -z "$(echo "$l" | filter_if_fast ../)" && -z "$(echo "$dl" | filter_if_fast ../)" ]] && exit 0

{
# Copy out the list of #defines we don't use, but it's OK.
sed -e '/^$/d' -e '/^#/d' < s_define.list

# Search the list of include files for #defines
# Ignore configuration objects #defines
# Ignore statistic "keys" generated for applications #defines
search=`cat $dl |
    sed -e '/API configuration keys: BEGIN/,/API configuration keys: END/d' \
        -e '/configuration section: BEGIN/,/configuration section: END/d' \
        -e '/Statistics section: BEGIN/,/Statistics section: END/d' |
    grep -E '^#define' |
    sed 's/#define[	 ][	 ]*\([A-Za-z_][A-Za-z0-9_]*\).*/\1/' |
    sort -u`

# Print the list of macros, followed by the occurrences: we're looking for
# macros that only appear once.
echo "$search"
cat $l | $FGREP -wo -h "$search"

} | sort | uniq -u

exit 0
