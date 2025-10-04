#! /usr/bin/env bash
set -o errexit
set -o verbose

echo "=== Starting Code Coverage Per Test script ==="

# Copy command line parameters into local variables
if test "$#" -ne 2; then
    echo "Two command line parameters (is_patch and num_jobs) are required"
    exit 1
fi
is_patch=$1
num_jobs=$2
echo "is_patch = ${is_patch}, and num_jobs = ${num_jobs}"

test/evergreen/find_cmake.sh

echo "Disk usage and free space for the current drive (pre-test):"
df -h .

# Create the output data directory.
mkdir -p coverage_data
mkdir -p coverage_report

# Setup the Python environment
virtualenv -p python3 venv
source venv/bin/activate
pip3 install lxml==4.8.0 Pygments==2.11.2 Jinja2==3.0.3 gcovr==5.0 pygit2==1.10.1 requests

######################################################
# Obtain the complexity metrics for the 'current' code
######################################################
# Install Metrix++, ensuring it is outside the 'src' directory
source test/evergreen/download_metrixpp.sh

# We only want complexity measures for the 'src' directory
cd src || exit 2
python3 "../metrixplusplus/metrix++.py" collect --std.code.lines.code --std.code.complexity.cyclomatic
python3 "../metrixplusplus/metrix++.py" export --db-file=metrixpp.db > ../coverage_report/metrixpp.csv
cd ..

python3 test/evergreen/code_coverage/per_test_code_coverage.py -v -c test/evergreen/code_coverage/code_coverage_config.json -b "$(pwd)/build_" -j "${num_jobs}" -g "$(pwd)/coverage_data" -s

echo "Disk usage and free space for the current drive (after completing coverage tests):"
df -h

# Clean up the build directory copies before creating the diff.
# This avoids the risk of having lots of copied 0 length files around which can, when excluded from
# the results, then generate a gcovr command line that is too long to be executed.
rm -Rf build*copy*

if [ "${is_patch}" = true ]; then
  echo "This is a patch build, so generate a diff and a report on reached functions"
  # Obtain the diff for the changes in this patch, excluding newly added 0-length files.
  python3 test/evergreen/code_change_report/git_diff_tool.py -g . -d coverage_report/diff.txt -v
  # Generate an HTML friendly version of the diff for
  sed 's/$/<br>/' coverage_report/diff.txt > coverage_report/diff.html
  # Logging for debugging
  echo "ls -l coverage_report"
  ls -l coverage_report
  echo "cat coverage_report/diff.txt"
  cat coverage_report/diff.txt
  python3 test/evergreen/code_change_report/per_test_code_coverage_report.py -c coverage_data -d coverage_report/diff.txt -m coverage_report/metrixpp.csv
fi

tar -czf coverage_report/coverage_data.tar.gz coverage_data

echo "Disk usage and free space for the current drive (post-test):"
df -h .
