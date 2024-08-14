#!/bin/bash

set -o errexit
set -o verbose

# Check if correct number of arguments are given, pr_args can be an empty string for patch builds.
if [[ $# -lt 3 || $# -gt 5 ]]; then
    echo "Error: invalid number of arguments."
    echo "Usage: coverage-report.sh \${is_patch} \${python_binary} \${github_commit} \${pr_args}"
    echo "Current args: $@"
    exit 1
fi

is_patch=$1
python_binary=$2
github_commit=$3
pr_args=$4

echo "coverage-report.sh"
echo "==========================="
echo "Current args:"
echo ". is_patch            =  $is_patch"
echo ". python_binary       =  $python_binary"
echo ". github_commit       =  $github_commit"
echo ". pr_args             =  $pr_args"

virtualenv -p $python_binary venv
source venv/bin/activate
pip3 install pygit2==1.10.1 requests==2.32.3

EXTRA_CODE_CHANGE_PARAMETERS=''

if [[ $is_patch == true ]]; then
    echo "This is a patch build"
    # Obtain the diff for the changes in this patch, excluding newly added 0-length files.
    python3 test/evergreen/code_change_report/git_diff_tool.py -g . -d coverage_report/diff.txt -v
    EXTRA_CODE_CHANGE_PARAMETERS='-d coverage_report/diff.txt'
    # Generate an HTML friendly version of the diff for
    sed 's/$/<br>/' coverage_report/diff.txt > coverage_report/diff.html
    # Logging for debugging
    ls -l coverage_report
    cat coverage_report/diff.txt
fi

######################################################
# Obtain the complexity metrics for the 'current' code
######################################################
# Install Metrix++, ensuring it is outside the 'src' directory
source test/evergreen/download_metrixpp.sh

# We only want complexity measures for the 'src' directory
cd src
python3 "../metrixplusplus/metrix++.py" collect --std.code.lines.code --std.code.complexity.cyclomatic
python3 "../metrixplusplus/metrix++.py" export --db-file=metrixpp.db > ../coverage_report/metrixpp.csv
cd ..

#######################################################
# Obtain the complexity metrics for the 'previous' code
#######################################################
git worktree add --detach wiredtiger_previous "${github_commit}"
cd wiredtiger_previous
if [[ $is_patch == true ]]; then
# Checkout the point at which this patch/branch diverged from develop
git checkout `python3 dist/common_functions.py last_commit_from_dev`
else
# Checkout the previous commit
git checkout HEAD~
fi

# Log the current git status of the 'previous' code
git status

cd src
python3 "../../metrixplusplus/metrix++.py" collect --std.code.lines.code --std.code.complexity.cyclomatic
python3 "../../metrixplusplus/metrix++.py" export --db-file=metrixpp.db > ../../coverage_report/metrixpp_prev.csv
cd ../..

#########################################
# Generate the change info and the report
#########################################

$python_binary test/evergreen/code_change_report/code_change_info.py -v -c coverage_report/full_coverage_report.json -g . $EXTRA_CODE_CHANGE_PARAMETERS -m coverage_report/metrixpp.csv -p coverage_report/metrixpp_prev.csv -o coverage_report/code_change_info.json

# Log the contents of the change info file
cat coverage_report/code_change_info.json

# Generate the Code Change Report
$python_binary test/evergreen/code_change_report/code_change_report.py -v -c coverage_report/code_change_info.json -o coverage_report/code_change_report.html $pr_args
