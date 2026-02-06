#!/bin/bash
# Copyright 2022 The Centipede Authors.
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

# Tested on Debian GNU/Linux 11 (bullseye)
#
# * git: to get the Centipede sources.
# * bazel: to build Centipede.
# * libssl-dev: to link Centipede (it uses SHA1).
# * binutils: Centipede uses objdump.
# * clang: to build Centipede and the targets.
#   For most of the functionality clang 11 or newer will work.
#   To get all of the functionality you may need to install fresh clang from
#   source: https://llvm.org/.
#   The functionality currently requiring fresh clang from source:
#     * -fsanitize-coverage=trace-loads
#     (https://clang.llvm.org/docs/SanitizerCoverage.html#tracing-data-flow)

set -eux -o pipefail
declare MAYBE_SUDO=""
if (( "$EUID" != 0 )); then
    MAYBE_SUDO="sudo"
fi

${MAYBE_SUDO} apt update

# Add Bazel distribution URI as a package source following:
# https://docs.bazel.build/versions/main/install-ubuntu.html
${MAYBE_SUDO} apt install -y curl gnupg apt-transport-https
curl -fsSL https://bazel.build/bazel-release.pub.gpg \
  | gpg --dearmor | ${MAYBE_SUDO} tee /etc/apt/trusted.gpg.d/bazel.gpg >/dev/null
echo "deb [arch=amd64] https://storage.googleapis.com/bazel-apt stable jdk1.8" \
  | ${MAYBE_SUDO} tee /etc/apt/sources.list.d/bazel.list >/dev/null
${MAYBE_SUDO} apt update

# Install LLVM, which provides llvm-symbolizer required for running Centipede in
# some modes.
${MAYBE_SUDO} apt install -y llvm

# Install other dependencies.
${MAYBE_SUDO} apt install -y git bazel binutils libssl-dev

# Get Clang-14, the earliest version that supports dataflow tracing:
#   * Download Clang from Chromium to support old OS (e.g. Ubuntu 16).
#   * Alternatively, download the fresh Clang from https://releases.llvm.org/
declare -r CLANG_URL="https://commondatastorage.googleapis.com/chromium-browser-clang/Linux_x64/clang-llvmorg-14-init-9436-g65120988-1.tgz"
declare -r CLANG_DIR="/tmp/clang"
mkdir "${CLANG_DIR}"
tar zxvf <(curl "${CLANG_URL}") -C "${CLANG_DIR}"
export CLANG_BIN_DIR="${CLANG_DIR}/bin"
