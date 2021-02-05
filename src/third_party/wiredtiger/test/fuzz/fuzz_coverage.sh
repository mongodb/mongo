#!/bin/bash
#
# fuzz_coverage.sh - generate coverage information after running a fuzz test.
#
# This script assumes it is running in the directory that the fuzz test was executed in and requires
# that WiredTiger was running with "-fprofile-instr-generate" and "-fcoverage-mapping".
#
# Usage
# fuzz_coverage.sh <fuzz-test-binary>
#
# Environment variables
# PROFDATA_BINARY --
# 	The binary to use to merge the profiling data. (default: llvm-profdata)
# COV_BINARY --
# 	The binary to use to generate coverage information. (default: llvm-cov)
#
# Output
# <fuzz-test-binary>_cov.txt --
#	A coverage report in text format. It can be inspect with the "less" command and searched for
#	functions of interest. The numbers on the left of each line of code indicate how many times
#	they were hit in the fuzz test.
# <fuzz-test-binary>_cov.html --
#	A coverage report in html format. If a web browser is available, this might be a nicer way
#	visualize the coverage.

if test "$#" -lt "1"; then
	echo "$0: must specify fuzz test to generate coverage for"
	exit 1
fi

fuzz_test_bin="$1"

if test -z "$PROFDATA_BINARY"; then
	profdata_bin="llvm-profdata"
	echo "$0: PROFDATA_BINARY is unset, defaulting to $profdata_bin"
else
	profdata_bin="$PROFDATA_BINARY"
fi

if test -z "$COV_BINARY"; then
	cov_bin="llvm-cov"
	echo "$0: COV_BINARY is unset, defaulting to $cov_bin"
else
	cov_bin="$COV_BINARY"
fi

# Remove anything from previous runs.
rm *_cov.profdata *_cov.txt *_cov.html &> /dev/null

fuzz_cov_name="${fuzz_test_bin}_cov"
combined_profdata_name="${fuzz_cov_name}.profdata"

# Check that there is coverage data.
ls *.profraw &> /dev/null
if test $? -ne "0"; then
	echo "$0: could not find any .profraw files in the current directory"
	echo "$0: ensure that -fprofile-instr-generate and -fcoverage-mapping are added to your CFLAGS and LINKFLAGS when configuring"
	exit 1
fi

$profdata_bin merge -sparse *.profraw -o $combined_profdata_name || exit 1
$cov_bin show $fuzz_test_bin -instr-profile=$combined_profdata_name > "${fuzz_cov_name}.txt"
$cov_bin show $fuzz_test_bin -instr-profile=$combined_profdata_name -format=html > "${fuzz_cov_name}.html"
