#!/bin/bash

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

set -eu

SCRIPT_DIR="$(cd -L "$(dirname "$0")" && echo "${PWD}")"
readonly SCRIPT_DIR

declare -r FUZZTEST_DIR="$(cd "${SCRIPT_DIR}/.." && echo "${PWD}")"
declare -r OUTPUT_ARTIFACTS_DIR="${FUZZTEST_DIR}/test-outputs"
# Must run under sudo, or else docker trips over insufficient permissions.
declare -r DOCKER_CMD="sudo docker"
declare -r DOCKER_IMAGE=debian

echo "Will save test output artifacts to $OUTPUT_ARTIFACTS_DIR"

${DOCKER_CMD} run \
  -v "${FUZZTEST_DIR}:/app" \
  -v "${OUTPUT_ARTIFACTS_DIR}:/output" \
  --env OUTPUT_ARTIFACTS_DIR=/output \
  -w /app \
  "${DOCKER_IMAGE}" \
  /app/centipede/run_test_workflow.sh
