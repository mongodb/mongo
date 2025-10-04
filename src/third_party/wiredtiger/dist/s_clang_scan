#!/bin/bash

. `dirname -- ${BASH_SOURCE[0]}`/common_functions.sh
cd_top
setup_trap 'rm -rf __wt.$$.* $t_build'

t_out=__wt.$$.out
t_out_filtered=__wt.$$.out_filtered
t_out_errors=__wt.$$.out_errors
t_diff_filtered=__wt.$$.diff_filtered
t_diff_result=__wt.$$.diff_result
t_build=__s_clang_scan_tmp_build

echo "$0: running scan-build in $PWD"

compiler=$(which clang)
scan=$(which scan-build)
echo "$0: compiler: $compiler"
echo "$0: scan-build: $scan"

# Check if Clang is available with the desired version.
desired_version="19.1.7"
if ! $compiler --version | grep -q $desired_version; then
    echo "$compiler is not using the expected version: $desired_version"
    $compiler --version
    exit 1
fi

# Remove old reports.
rm -rf clangScanBuildReports && mkdir clangScanBuildReports
rm -rf ${t_build} && mkdir ${t_build}

cd ${t_build} || exit 1
args="$args --use-cc=$compiler"
args="$args -disable-checker core.NullDereference"
$scan $args cmake -G Ninja ../. > /dev/null
$scan $args ninja wt > ../$t_out 2>&1
cd ..

# Strip comments from the diff file.
cat dist/s_clang_scan.diff | sed '/^#/d' | LC_ALL=C sort > $t_diff_filtered

# Compare the set of errors/warnings and exit success when they're expected.
# clang_scan outputs errors/warnings with a leading file path followed by a
# colon, followed by the string "error:" or "warning:".
cat $t_out |
    grep -E '^[a-z\./].*error: |^[a-z\./].*warning: ' |
    sed -e 's/^.*wiredtiger/wiredtiger/' > $t_out_errors
cat $t_out_errors |
    sed -e 's/:.*:.*://' |
    LC_ALL=C sort > $t_out_filtered

diff $t_diff_filtered $t_out_filtered > $t_diff_result && exit 0
echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
echo 'scan-build output differs from expected:'
echo '<<<<<<<<<< Expected >>>>>>>>>> Current'
echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
cat $t_diff_result
echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="

# Since we compare lines after stripping certain parts, it can be difficult to identify
# the original source of the warnings. To recover the line numbers, we search for the
# unexpected diff entries in the original output.
echo 'that could be related to the following output entries:'
echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
grep '^>' "$t_diff_result" | sed 's/^> *//' | uniq | while read line; do
    path=${line%% *} # Cut off all at the first space
    msg=${line#* } # Cut off all up to the first space
    grep -F "$path:" "$t_out_errors" | grep -F "$msg"
done
echo "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-="
exit 1
