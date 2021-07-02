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

source "${ABSEIL_ROOT}/ci/cmake_common.sh"

if [[ -z ${ABSL_CMAKE_CXX_STANDARDS:-} ]]; then
  ABSL_CMAKE_CXX_STANDARDS="11 14 17 20"
fi

if [[ -z ${ABSL_CMAKE_BUILD_TYPES:-} ]]; then
  ABSL_CMAKE_BUILD_TYPES="Debug Release"
fi

if [[ -z ${ABSL_CMAKE_BUILD_SHARED:-} ]]; then
  ABSL_CMAKE_BUILD_SHARED="OFF ON"
fi

source "${ABSEIL_ROOT}/ci/linux_docker_containers.sh"
readonly DOCKER_CONTAINER=${LINUX_GCC_LATEST_CONTAINER}

for std in ${ABSL_CMAKE_CXX_STANDARDS}; do
  for compilation_mode in ${ABSL_CMAKE_BUILD_TYPES}; do
    for build_shared in ${ABSL_CMAKE_BUILD_SHARED}; do
      time docker run \
        --mount type=bind,source="${ABSEIL_ROOT}",target=/abseil-cpp,readonly \
        --tmpfs=/buildfs:exec \
        --workdir=/buildfs \
        --cap-add=SYS_PTRACE \
        --rm \
        -e CFLAGS="-Werror" \
        -e CXXFLAGS="-Werror" \
        ${DOCKER_EXTRA_ARGS:-} \
        "${DOCKER_CONTAINER}" \
        /bin/bash -c "
          cmake /abseil-cpp \
            -DABSL_GOOGLETEST_DOWNLOAD_URL=${ABSL_GOOGLETEST_DOWNLOAD_URL} \
            -DBUILD_SHARED_LIBS=${build_shared} \
            -DBUILD_TESTING=ON \
            -DCMAKE_BUILD_TYPE=${compilation_mode} \
            -DCMAKE_CXX_STANDARD=${std} \
            -DCMAKE_MODULE_LINKER_FLAGS=\"-Wl,--no-undefined\" && \
          make -j$(nproc) && \
          ctest -j$(nproc) --output-on-failure"
    done
  done
done
