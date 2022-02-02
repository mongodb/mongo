#!/bin/bash
#
# fuzz_run.sh - run a fuzz test.
#
# This script will emit all data in the current working directory including: fuzzing logs, home
# directories and profiling data (if we've compiled with Clang coverage).
#
# Running fuzzers compiled with ASan (-fsanitize=address) is recommended. If you want to also run
# calculate coverage, you should also add "-fprofile-instr-generate" and "-fcoverage-mapping" to
# your CFLAGS and LINKFLAGS when configuring.
#
# Usage
# fuzz_run.sh <fuzz-test-binary> [fuzz-test-args]
#
# If the fuzzer you're running has an existing corpus directory, you may want to run with the corpus
# supplied:
# e.g. fuzz_run.sh ../../build/test/fuzz/fuzz_config corpus/
#
# Output
# crash-<input-hash> --
#	If an error occurs, a file will be produced containing the input that crashed the target.
# fuzz-N.log --
#	The LibFuzzer log for worker N. This is just an ID that LibFuzzer assigns to each worker
#	ranging from 0 => the number of workers - 1.
# WT_TEST_<pid> --
#	The home directory for a given worker process.
# WT_TEST_<pid>.profraw --
#	If a fuzzer is running with Clang coverage, files containing profiling data for a given
#	worker will be produced. These will be used by fuzz_coverage.

if test "$#" -lt "1"; then
	echo "$0: must specify fuzz test to run"
	exit 1
fi

# Take the binary name and shift.
# We don't want to forward this as an argument.
fuzz_test_bin="$1"
shift

# Remove anything from previous runs.
rm -rf WT_TEST_* &> /dev/null
rm *.profraw fuzz-*.log &> /dev/null

# If we've compiled to emit coverage information, each worker process should write their own
# performance data.
export LLVM_PROFILE_FILE="WT_TEST_%p.profraw"

# The rationale for each flag is below:
# - jobs=8
#	Choosing 8 workers is a reasonable starting point. Depending on their machine, they can bump
#	this number up but most machines will be able to handle this and it finishes jobs much faster
#	than without this flag (equivalent to jobs=1).
# - runs=100000000
#	Do 100 million runs to make sure that we're stressing the system and hitting lots of
#	branches. Ideally, we'd just let the fuzzer run until the process is killed by the user but
#	unfortunately, coverage data won't get written out in that case.
# - close_fd_mask=3
#	Suppress stdout and stderr. This isn't ideal but any fuzzing target that prints an error
#	will quickly fill up your disk. Better to just replay the input without this flag if you
#	uncover a bug.
$fuzz_test_bin -jobs=8 -runs=100000000 -close_fd_mask=3 "$@"
