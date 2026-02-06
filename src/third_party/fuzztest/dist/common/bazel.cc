// Copyright 2024 The Centipede Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./common/bazel.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_cat.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace fuzztest::internal {

namespace {

absl::Duration GetBazelTestTimeout() {
  const char *test_timeout_env = std::getenv("TEST_TIMEOUT");
  if (test_timeout_env == nullptr) return absl::InfiniteDuration();
  // When using `bazel run`, `TEST_TIMEOUT` is set but not used, and we should
  // ignore the timeout. We detect this by the presence of
  // `BUILD_WORKSPACE_DIRECTORY`
  // (https://bazel.build/docs/user-manual#running-executables).
  if (std::getenv("BUILD_WORKSPACE_DIRECTORY") != nullptr) {
    return absl::InfiniteDuration();
  }
  int timeout_s = 0;
  CHECK(absl::SimpleAtoi(test_timeout_env, &timeout_s))
      << "Failed to parse TEST_TIMEOUT: \"" << test_timeout_env << "\"";
  return absl::Seconds(timeout_s);
}

}  // namespace

TestShard GetBazelTestShard() {
  static TestShard cached_test_shard = [] {
    TestShard test_shard;
    if (const char *test_total_shards_env = std::getenv("TEST_TOTAL_SHARDS");
        test_total_shards_env != nullptr) {
      CHECK(absl::SimpleAtoi(test_total_shards_env, &test_shard.total_shards))
          << "Failed to parse TEST_TOTAL_SHARDS as an integer: \""
          << test_total_shards_env << "\"";
      CHECK_GT(test_shard.total_shards, 0)
          << "TEST_TOTAL_SHARDS must be greater than 0.";
    }
    if (const char *test_shard_index_env = std::getenv("TEST_SHARD_INDEX");
        test_shard_index_env != nullptr) {
      CHECK(absl::SimpleAtoi(test_shard_index_env, &test_shard.index))
          << "Failed to parse TEST_SHARD_INDEX as an integer: \""
          << test_shard_index_env << "\"";
      CHECK(0 <= test_shard.index && test_shard.index < test_shard.total_shards)
          << "TEST_SHARD_INDEX must be in the range [0, "
          << test_shard.total_shards << ").";
    }
    return test_shard;
  }();
  return cached_test_shard;
}

absl::Status VerifyBazelHasEnoughTimeToRunTest(absl::Time target_start_time,
                                               absl::Duration test_time_limit,
                                               int executed_tests_in_shard,
                                               int fuzz_test_count) {
  static const absl::Duration bazel_test_timeout = GetBazelTestTimeout();
  const int shard_count = GetBazelTestShard().total_shards;
  constexpr float kTimeoutSafetyFactor = 1.2;
  const auto required_test_time = kTimeoutSafetyFactor * test_time_limit;
  const auto remaining_duration =
      bazel_test_timeout - (absl::Now() - target_start_time);
  if (required_test_time <= remaining_duration) return absl::OkStatus();
  std::string error =
      "Cannot fuzz a fuzz test within the given timeout. Please ";
  if (executed_tests_in_shard == 0) {
    // Increasing number of shards won't help.
    const absl::Duration suggested_timeout =
        required_test_time * ((fuzz_test_count - 1) / shard_count + 1);
    absl::StrAppend(&error, "set the `timeout` to ", suggested_timeout,
                    " or reduce the fuzzing time, ");
  } else {
    constexpr int kMaxShardCount = 50;
    const int suggested_shard_count = std::min(
        (fuzz_test_count - 1) / executed_tests_in_shard + 1, kMaxShardCount);
    const int suggested_tests_per_shard =
        (fuzz_test_count - 1) / suggested_shard_count + 1;
    if (suggested_tests_per_shard > executed_tests_in_shard) {
      // We wouldn't be able to execute the suggested number of tests without
      // timeout. This case can only happen if we would in fact need more than
      // `kMaxShardCount` shards, indicating that there are simply too many fuzz
      // tests in a binary.
      CHECK_EQ(suggested_shard_count, kMaxShardCount);
      absl::StrAppend(&error,
                      "split the fuzz tests into several test binaries where "
                      "each binary has at most ",
                      executed_tests_in_shard * kMaxShardCount, "tests ",
                      "with `shard_count` = ", kMaxShardCount, ", ");
    } else {
      // In this case, `suggested_shard_count` must be greater than
      // `shard_count`, otherwise we would have already executed all the tests
      // without a timeout.
      CHECK_GT(suggested_shard_count, shard_count);
      absl::StrAppend(&error, "increase the `shard_count` to ",
                      suggested_shard_count, ", ");
    }
  }
  absl::StrAppend(&error, "to avoid this issue. ");
  absl::StrAppend(&error,
                  "(https://bazel.build/reference/be/"
                  "common-definitions#common-attributes-tests)");
  return absl::ResourceExhaustedError(error);
}

}  // namespace fuzztest::internal
