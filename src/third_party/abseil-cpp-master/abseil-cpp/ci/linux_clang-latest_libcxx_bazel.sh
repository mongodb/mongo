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

# This script that can be invoked to test abseil-cpp in a hermetic environment
# using a Docker image on Linux. You must have Docker installed to use this
# script.

set -euox pipefail

if [[ -z ${ABSEIL_ROOT:-} ]]; then
  ABSEIL_ROOT="$(realpath $(dirname ${0})/..)"
fi

if [[ -z ${STD:-} ]]; then
  STD="c++11 c++14 c++17 c++20"
fi

if [[ -z ${COMPILATION_MODE:-} ]]; then
  COMPILATION_MODE="fastbuild opt"
fi

if [[ -z ${EXCEPTIONS_MODE:-} ]]; then
  EXCEPTIONS_MODE="-fno-exceptions -fexceptions"
fi

source "${ABSEIL_ROOT}/ci/linux_docker_containers.sh"
readonly DOCKER_CONTAINER=${LINUX_CLANG_LATEST_CONTAINER}

# USE_BAZEL_CACHE=1 only works on Kokoro.
# Without access to the credentials this won't work.
if [[ ${USE_BAZEL_CACHE:-0} -ne 0 ]]; then
  DOCKER_EXTRA_ARGS="--mount type=bind,source=${KOKORO_KEYSTORE_DIR},target=/keystore,readonly ${DOCKER_EXTRA_ARGS:-}"
  # Bazel doesn't track changes to tools outside of the workspace
  # (e.g. /usr/bin/gcc), so by appending the docker container to the
  # remote_http_cache url, we make changes to the container part of
  # the cache key. Hashing the key is to make it shorter and url-safe.
  container_key=$(echo ${DOCKER_CONTAINER} | sha256sum | head -c 16)
  BAZEL_EXTRA_ARGS="--remote_http_cache=https://storage.googleapis.com/absl-bazel-remote-cache/${container_key} --google_credentials=/keystore/73103_absl-bazel-remote-cache ${BAZEL_EXTRA_ARGS:-}"
fi

# Avoid depending on external sites like GitHub by checking --distdir for
# external dependencies first.
# https://docs.bazel.build/versions/master/guide.html#distdir
if [[ ${KOKORO_GFILE_DIR:-} ]] && [[ -d "${KOKORO_GFILE_DIR}/distdir" ]]; then
  DOCKER_EXTRA_ARGS="--mount type=bind,source=${KOKORO_GFILE_DIR}/distdir,target=/distdir,readonly ${DOCKER_EXTRA_ARGS:-}"
  BAZEL_EXTRA_ARGS="--distdir=/distdir ${BAZEL_EXTRA_ARGS:-}"
fi

for std in ${STD}; do
  for compilation_mode in ${COMPILATION_MODE}; do
    for exceptions_mode in ${EXCEPTIONS_MODE}; do
      echo "--------------------------------------------------------------------"
      time docker run \
        --mount type=bind,source="${ABSEIL_ROOT}",target=/abseil-cpp-ro,readonly \
        --tmpfs=/abseil-cpp \
        --workdir=/abseil-cpp \
        --cap-add=SYS_PTRACE \
        --rm \
        -e CC="/opt/llvm/clang/bin/clang" \
        -e BAZEL_CXXOPTS="-std=${std}:-nostdinc++" \
        -e BAZEL_LINKOPTS="-L/opt/llvm/libcxx/lib:-lc++:-lc++abi:-lm:-Wl,-rpath=/opt/llvm/libcxx/lib" \
        -e CPLUS_INCLUDE_PATH="/opt/llvm/libcxx/include/c++/v1" \
        ${DOCKER_EXTRA_ARGS:-} \
        ${DOCKER_CONTAINER} \
        /bin/sh -c "
          cp -r /abseil-cpp-ro/* /abseil-cpp/
          if [ -n \"${ALTERNATE_OPTIONS:-}\" ]; then
            cp ${ALTERNATE_OPTIONS:-} absl/base/options.h || exit 1
          fi
          /usr/local/bin/bazel test ... \
            --compilation_mode=\"${compilation_mode}\" \
            --copt=\"${exceptions_mode}\" \
            --copt=-Werror \
            --define=\"absl=1\" \
            --keep_going \
            --show_timestamps \
            --test_env=\"GTEST_INSTALL_FAILURE_SIGNAL_HANDLER=1\" \
            --test_env=\"TZDIR=/abseil-cpp/absl/time/internal/cctz/testdata/zoneinfo\" \
            --test_output=errors \
            --test_tag_filters=-benchmark \
            ${BAZEL_EXTRA_ARGS:-}"
    done
  done
done
