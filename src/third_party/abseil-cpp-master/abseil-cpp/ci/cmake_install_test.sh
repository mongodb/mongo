#!/bin/bash
#
# Copyright 2019 The Abseil Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#    https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -euox pipefail

if [[ -z ${ABSEIL_ROOT:-} ]]; then
  ABSEIL_ROOT="$(realpath $(dirname ${0})/..)"
fi

if [[ -z ${LINK_TYPE:-} ]]; then
  LINK_TYPE="STATIC DYNAMIC"
fi

source "${ABSEIL_ROOT}/ci/cmake_common.sh"

source "${ABSEIL_ROOT}/ci/linux_docker_containers.sh"
readonly DOCKER_CONTAINER=${LINUX_GCC_LATEST_CONTAINER}

for link_type in ${LINK_TYPE}; do
  time docker run \
    --mount type=bind,source="${ABSEIL_ROOT}",target=/abseil-cpp-ro,readonly \
    --tmpfs=/buildfs:exec \
    --tmpfs=/abseil-cpp:exec \
    --workdir=/abseil-cpp \
    --cap-add=SYS_PTRACE \
    -e "LINK_TYPE=${link_type}" \
    --rm \
    ${DOCKER_CONTAINER} \
    /bin/bash -c "cp -r /abseil-cpp-ro/* . && CMake/install_test_project/test.sh"
done
