#
# Copyright 2018 The Abseil Authors.
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
#

absl_cc_library(
  NAME
    hash
  HDRS
    "hash.h"
  SRCS
    "internal/hash.cc"
    "internal/hash.h"
  COPTS
    ${ABSL_DEFAULT_COPTS}
    DEPS
    absl::city
    absl::config
    absl::core_headers
    absl::endian
    absl::fixed_array
    absl::meta
    absl::int128
    absl::strings
    absl::optional
    absl::variant
    absl::utility
    absl::wyhash
  PUBLIC
)

absl_cc_library(
  NAME
    hash_testing
  HDRS
    "hash_testing.h"
  COPTS
    ${ABSL_TEST_COPTS}
  DEPS
    absl::spy_hash_state
    absl::meta
    absl::strings
    absl::variant
    gmock
  TESTONLY
)

absl_cc_test(
  NAME
    hash_test
  SRCS
    "hash_test.cc"
  COPTS
    ${ABSL_TEST_COPTS}
  DEPS
    absl::cord_test_helpers
    absl::hash
    absl::hash_testing
    absl::core_headers
    absl::flat_hash_set
    absl::spy_hash_state
    absl::meta
    absl::int128
    gmock_main
)

absl_cc_library(
  NAME
    spy_hash_state
  HDRS
    "internal/spy_hash_state.h"
  COPTS
    ${ABSL_DEFAULT_COPTS}
  DEPS
    absl::hash
    absl::strings
    absl::str_format
  TESTONLY
)

absl_cc_library(
  NAME
    city
  HDRS
    "internal/city.h"
  SRCS
    "internal/city.cc"
  COPTS
    ${ABSL_DEFAULT_COPTS}
  DEPS
    absl::config
    absl::core_headers
    absl::endian
)

absl_cc_test(
  NAME
    city_test
  SRCS
    "internal/city_test.cc"
  COPTS
    ${ABSL_TEST_COPTS}
  DEPS
    absl::city
    gmock_main
)

absl_cc_library(
  NAME
    wyhash
  HDRS
    "internal/wyhash.h"
  SRCS
    "internal/wyhash.cc"
  COPTS
    ${ABSL_DEFAULT_COPTS}
  DEPS
    absl::config
    absl::endian
    absl::int128
)

absl_cc_test(
  NAME
    wyhash_test
  SRCS
    "internal/wyhash_test.cc"
  COPTS
    ${ABSL_TEST_COPTS}
  DEPS
    absl::wyhash
    absl::strings
    gmock_main
)
