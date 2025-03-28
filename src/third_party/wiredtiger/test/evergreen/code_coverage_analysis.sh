#!/bin/bash

set -o errexit
set -o verbose

print_usage() {
    echo "Error: invalid number of arguments."
    echo "Usage: code_coverage_analysis.sh \${coverage_filter} \${num_jobs} \${python_binary} \${generate_atlas_format} \${combine_coverage_report} \${first_coverage_file_path} \${second_coverage_file_path}"
    echo "Current args: $@"
}

# Check if enough arguments are given, first_coverage_file_path and second_coverage_file_path are not defined if combine_coverage_report is false.
if [ $# -lt 3 ]; then
    print_usage $@
    exit 1
fi

coverage_filter=$1
num_jobs=$2
python_binary=$3
generate_atlas_format=$4
combine_coverage_report=$5
first_coverage_file_path=$6
second_coverage_file_path=$7

if [[ "$combine_coverage_report" == "True" && $# -ne 7 ]]; then
    print_usage $@
    exit 1
fi

virtualenv -p python3 venv
source venv/bin/activate
pip3 install lxml==4.8.0 Pygments==2.11.2 Jinja2==3.0.3 gcovr==5.0
mkdir -p coverage_report
output_flags="--html-self-contained --html-details coverage_report/2_coverage_report.html --json-summary-pretty --json-summary coverage_report/1_coverage_report_summary.json --json coverage_report/full_coverage_report.json"
if [ ! -z $combine_coverage_report ]; then
  gcovr --gcov-ignore-parse-errors -f $coverage_filter --add-tracefile $first_coverage_file_path --add-tracefile $second_coverage_file_path -j $num_jobs $output_flags
else
  gcovr --gcov-ignore-parse-errors -f $coverage_filter -j $num_jobs $output_flags
  $python_binary test/evergreen/code_coverage_analysis.py -s coverage_report/1_coverage_report_summary.json -t time.txt
fi

# Generate Atlas compatible format report.
if [ ! -z $generate_atlas_format ]; then
    $python_binary test/evergreen/code_coverage_analysis.py -c component_coverage -o coverage_report/atlas_out_code_coverage.json -s coverage_report/1_coverage_report_summary.json
fi
