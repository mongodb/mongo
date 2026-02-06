#!/bin/bash

# Copyright 2023 The Centipede Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# The caller must source ./test_util.sh before calling any functions below.
#
# TODO(ussuri): Merge with the versions in testing/centipede_main_cns_test.sh?

# Performs some basic fuzzing runs of target "$1".
function fuzztest::internal::run_some_fuzzing() {
  local -r target=($1)

  FUNC="${FUNCNAME[0]}"
  WD="${TEST_TMPDIR}/${FUNC}/WD"
  LOG="${TEST_TMPDIR}/${FUNC}/log"
  fuzztest::internal::ensure_empty_dir "${WD}"

  echo "============ ${FUNC}: First run: 100 runs in batches of 7"
  ${target} -workdir="${WD}" -num_runs 100 --batch_size=7 | tee "${LOG}"
  fuzztest::internal::assert_regex_in_file '\[S0.100\] end-fuzz:' "${LOG}" # Check the number of runs.
  fuzztest::internal::assert_fuzzing_success "${LOG}"
  ls -l "${WD}"

  echo "============ ${FUNC}: Second run: 300 runs in batches of 8"
  ${target} -workdir="${WD}" -num_runs 300 --batch_size=8 | tee "${LOG}"
  fuzztest::internal::assert_regex_in_file '\[S0.300\] end-fuzz:' "${LOG}" # Check the number of runs.
  fuzztest::internal::assert_fuzzing_success "${LOG}"
  ls -l "${WD}"

  N_SHARDS=3
  echo "============ ${FUNC}: Running ${N_SHARDS} shards"
  for ((s = 0; s < "${N_SHARDS}"; s++)); do
    ${target} --workdir="${WD}" -num_runs 100 --first_shard_index="$s" \
      --total_shards="${N_SHARDS}" | tee "${LOG}.${s}" &
  done
  wait
  echo "============ ${FUNC}: Shards finished, checking output"
  for ((s = 0; s < "${N_SHARDS}"; s++)); do
    grep -q "centipede.cc.*end-fuzz:" "${LOG}.${s}"
  done
  fuzztest::internal::assert_fuzzing_success "${LOG}".*

  ls -l "${WD}"
}

# Tests fuzzing with a target "$1" that crashes with a nice_input "$2" and
# a crash_input "$3". The log should contain string "$4".
function fuzztest::internal::test_crashing_target() {
  local -r target=($1)
  local -r nice_input="$2"
  local -r crash_input="$3"
  local -r regex="$4"

  FUNC="${FUNCNAME[0]}"
  WD="${TEST_TMPDIR}/${FUNC}/WD"
  TMPCORPUS="${TEST_TMPDIR}/${FUNC}/C"
  LOG="${TEST_TMPDIR}/${FUNC}/log"
  fuzztest::internal::ensure_empty_dir "${WD}"
  fuzztest::internal::ensure_empty_dir "${TMPCORPUS}"

  # Create a corpus with one crasher and one other input.
  echo -n "${crash_input}" >"${TMPCORPUS}/${crash_input}" # induces abort in the target.
  echo -n "${nice_input}" >"${TMPCORPUS}/${nice_input}"     # just some input.
  ${target} --workdir="${WD}" --corpus_from_files="${TMPCORPUS}"

  # Run fuzzing with num_runs=0, i.e. only run the inputs from the corpus.
  # Expecting a crash to be observed and reported.
  ${target} --workdir="${WD}" --num_runs=0 | tee "${LOG}"
  fuzztest::internal::assert_regex_in_file "2 inputs to rerun" "${LOG}"
  fuzztest::internal::assert_regex_in_file "Batch execution failed:" "${LOG}"

  # Comes from test_fuzz_target.cc
  fuzztest::internal::assert_regex_in_file "${regex}" "${LOG}"
}

# Replays inputs with the specified target.
#
# Arguments:
#   $1 - The target to replay the tests for.
#   $2 - The log string regex that should be present in the test results.
#   $@ - The remaining inputs to be used for the tests.
function fuzztest::internal::test_replaying_target() {
  local -r target="$1"; shift
  local -r regex="$1"; shift
  local -ra corpus_elts=("$@")

  FUNC="${FUNCNAME[0]}"
  WD="${TEST_TMPDIR}/${FUNC}/WD"
  TMPCORPUS="${TEST_TMPDIR}/${FUNC}/C"
  LOG="${TEST_TMPDIR}/${FUNC}/log"
  fuzztest::internal::ensure_empty_dir "${WD}"
  fuzztest::internal::ensure_empty_dir "${TMPCORPUS}"

  local -i n=0
  for elt in "${corpus_elts[@]}"; do
    echo -n "${elt}" >"${TMPCORPUS}/corpus.$n"
    (( ++n ))
  done

  ${target} --workdir="${WD}" --corpus_from_files="${TMPCORPUS}"

  # Run fuzzing with num_runs=0, i.e. only run the inputs from the corpus.
  ${target} --workdir="${WD}" --num_runs=0 | tee "${LOG}"
  fuzztest::internal::assert_regex_in_file "${regex}" "${LOG}"
}
