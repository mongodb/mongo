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

# Verify that the `:centipede` build target indeed creates the expected binary.

set -eu

source "$(dirname "$0")/test_util.sh"

centipede_test_srcdir="$(fuzztest::internal::get_centipede_test_srcdir)"
centipede_binary="${centipede_test_srcdir}/centipede"
if ! [[ -x "${centipede_binary}" ]]; then
  die "Build target ':centipede' failed to create expected executable \
${centipede_binary}"
fi
