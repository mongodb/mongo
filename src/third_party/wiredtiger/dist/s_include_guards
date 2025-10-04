#!/bin/bash

# Check if every .h file has an include guard of any style:
# either "#ifndef/#define {FILE}_H" or "#pragma once".

. `dirname -- ${BASH_SOURCE[0]}`/common_functions.sh
cd_top
check_fast_mode_flag

for f in $(
    find bench ext src test tools -name \*.h |
    grep -F -v wtperf_opt_inline.h |
    filter_if_fast
); do
  if ! grep -qE '#pragma\s+once|#define[^\n]+_H[_]*$' $f; then
    echo "Please add include guards in: $f"
  fi
done
