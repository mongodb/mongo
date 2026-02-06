#!/bin/bash

# Copyright 2018 Google LLC
# Copyright 2022 Centipede Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

set -eu -o pipefail

source ./centipede/install_dependencies_debian.sh
# The above script installs a custom version of clang and exports its bin subdir
# in CLANG_BIN_DIR envvar.
export PATH="${CLANG_BIN_DIR}:$PATH"
declare MAYBE_SUDO=""
if (( "$EUID" != 0 )); then
    MAYBE_SUDO="sudo"
fi

${MAYBE_SUDO} apt install -y rename

########################################
# LOG ENVIRONMENT DEBUG INFO
########################################
date --rfc-3339=seconds
echo "Debug Info"
echo "OUTPUT_ARTIFACTS_DIR=${OUTPUT_ARTIFACTS_DIR}"

mkdir -p "${OUTPUT_ARTIFACTS_DIR}"
find . >"${OUTPUT_ARTIFACTS_DIR}/full_file_list.log"
{
  printenv
  which bazel
  bazel version
  bazel info --show_make_env
} >"${OUTPUT_ARTIFACTS_DIR}/build_environment.log"

########################################
# RUN TESTS
########################################
date --rfc-3339=seconds
echo "Building and testing with Bazel"

declare BAZEL_OUTPUT_DIR
BAZEL_OUTPUT_DIR="$(bazel info output_base)"
readonly BAZEL_OUTPUT_DIR
declare -ra BAZEL_ARGS=("--color=no" "--curses=no" "--noshow_progress")

set +e
# TODO(b/259298232): As remaining parts of the bug get fixed, do:
#  - Remove `--local_test_jobs=1`.
#  - When all tests under `testing` pass, remove separate tests for each
#    subdirectory and replace `centipede:all` with `centipede/...`.
#  - Use a single `bazel test "${BAZEL_ARGS[@]}" ...`.
bazel test "${BAZEL_ARGS[@]}" --local_test_jobs=1 --test_output=streamed \
  centipede:all &&
bazel test "${BAZEL_ARGS[@]}" centipede/testing:instrumentation_test centipede/testing:runner_test &&
bazel test "${BAZEL_ARGS[@]}" centipede/puzzles:all
bazel test "${BAZEL_ARGS[@]}" --linkopt=-fsanitize=address --copt=-fsanitize=address centipede/puzzles:all
bazel test "${BAZEL_ARGS[@]}" --no//centipede:use_riegeli --local_test_jobs=1 --test_output=streamed centipede:all &&
bazel test "${BAZEL_ARGS[@]}" --no//centipede:use_riegeli centipede/testing:instrumentation_test centipede/testing:runner_test &&
bazel test "${BAZEL_ARGS[@]}" --no//centipede:use_riegeli centipede/puzzles:all
bazel test "${BAZEL_ARGS[@]}" --no//centipede:use_riegeli --linkopt=-fsanitize=address --copt=-fsanitize=address centipede/puzzles:all

declare -ri exit_code=$?
set -e

# Capture the build log as a fake test to reduce download spam
declare -r FULL_BUILD_LOG_DIR="bazel_full_build_log"
mkdir -p "${OUTPUT_ARTIFACTS_DIR}/${FULL_BUILD_LOG_DIR}"
cp "${BAZEL_OUTPUT_DIR}/command.log" "${OUTPUT_ARTIFACTS_DIR}/${FULL_BUILD_LOG_DIR}/sponge_log.log"
cat >"${OUTPUT_ARTIFACTS_DIR}/${FULL_BUILD_LOG_DIR}/sponge_log.xml" <<DOC
<?xml version="1.0" encoding="UTF-8"?>
<testsuites name="">
  <testsuite name="${FULL_BUILD_LOG_DIR}" tests="1" errors="${exit_code}"></testsuite>
</testsuites>
DOC
chmod -R a+w "${OUTPUT_ARTIFACTS_DIR}/${FULL_BUILD_LOG_DIR}"

########################################
# REMAP OUTPUT FILES
########################################
declare -r OUTPUT_BAZEL_LOGS_DIR="${OUTPUT_ARTIFACTS_DIR}/bazel_test_logs"
rm -rf "${OUTPUT_BAZEL_LOGS_DIR}"  # For local testing
mkdir -p "${OUTPUT_BAZEL_LOGS_DIR}"

# Copy test.{log,xml} files to kokoro artifacts directory, then rename them.
find -L bazel-testlogs -name "test.log" -exec cp --parents {} "${OUTPUT_BAZEL_LOGS_DIR}" \;
find -L "${OUTPUT_BAZEL_LOGS_DIR}" -name "test.log" -exec rename 's/test\.log/sponge_log.log/' {} \;
find -L bazel-testlogs -name "test.xml" -exec cp --parents {} "${OUTPUT_BAZEL_LOGS_DIR}" \;
find -L "${OUTPUT_BAZEL_LOGS_DIR}" -name "test.xml" -exec rename 's/test\.xml/sponge_log.xml/' {} \;

chmod -R a+w "${OUTPUT_BAZEL_LOGS_DIR}"

date --rfc-3339=seconds
echo "Exiting test workflow"

exit ${exit_code}
