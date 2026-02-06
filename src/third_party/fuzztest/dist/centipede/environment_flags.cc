// Copyright 2023 The Centipede Authors.
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

#include "./centipede/environment_flags.h"

#include <cstdlib>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/strings/match.h"
#include "absl/strings/str_split.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "./centipede/environment.h"
#include "./common/logging.h"

using ::fuzztest::internal::Environment;

#define CENTIPEDE_FLAG(TYPE, NAME, DEFAULT, DESC) \
  ABSL_FLAG(TYPE, NAME, DEFAULT, DESC);
#include "./centipede/centipede_flags.inc"
#undef CENTIPEDE_FLAG

#define CENTIPEDE_FLAG_ALIAS(ALIAS_NAME, ORIGINAL_NAME)                 \
  ABSL_FLAG(decltype(Environment::Default().ORIGINAL_NAME), ALIAS_NAME, \
            Environment::Default().ORIGINAL_NAME,                       \
            "Alias of --" #ORIGINAL_NAME)                               \
      .OnUpdate([]() {                                                  \
        absl::SetFlag(&FLAGS_##ORIGINAL_NAME,                           \
                      absl::GetFlag(FLAGS_##ALIAS_NAME));               \
      });
CENTIPEDE_FLAG_ALIAS(first_shard_index, my_shard_index)
CENTIPEDE_FLAG_ALIAS(timeout, timeout_per_input)
CENTIPEDE_FLAG_ALIAS(num_crash_reports, max_num_crash_reports)
CENTIPEDE_FLAG_ALIAS(minimize_crash, minimize_crash_file_path)
#undef CENTIPEDE_FLAG_ALIAS

ABSL_FLAG(absl::Duration, stop_after, absl::InfiniteDuration(),
          "Equivalent to setting --stop_at to the current date/time + this "
          "duration. These two flags are mutually exclusive.");
ABSL_RETIRED_FLAG(size_t, distill_shards, 0,
                  "No longer supported: use --distill instead.");

namespace fuzztest::internal {

namespace {

// Computes the final stop-at time based on the possibly user-provided inputs.
absl::Time GetStopAtTime(absl::Time stop_at, absl::Duration stop_after) {
  const bool stop_at_is_non_default = stop_at != absl::InfiniteFuture();
  const bool stop_after_is_non_default = stop_after != absl::InfiniteDuration();
  CHECK_LE(stop_at_is_non_default + stop_after_is_non_default, 1)
      << "At most one of --stop_at and --stop_after should be specified, "
         "including via --config file: "
      << VV(stop_at) << VV(stop_after);
  if (stop_at_is_non_default) {
    return stop_at;
  } else if (stop_after_is_non_default) {
    return absl::Now() + stop_after;
  } else {
    return absl::InfiniteFuture();
  }
}

}  // namespace

Environment CreateEnvironmentFromFlags(const std::vector<std::string> &argv) {
  Environment env_from_flags = {
#define CENTIPEDE_FLAG(_TYPE, NAME, _DEFAULT, _DESC) \
  absl::GetFlag(FLAGS_##NAME),
#include "./centipede/centipede_flags.inc"
#undef CENTIPEDE_FLAG
  };

  env_from_flags.stop_at =
      GetStopAtTime(env_from_flags.stop_at, absl::GetFlag(FLAGS_stop_after));

  if (env_from_flags.coverage_binary.empty()) {
    env_from_flags.coverage_binary =
        *absl::StrSplit(env_from_flags.binary, ' ').begin();
  }
  env_from_flags.binary_name =
      std::filesystem::path(env_from_flags.coverage_binary).filename().string();
  env_from_flags.UpdateBinaryHashIfEmpty();

  env_from_flags.UpdateTimeoutPerBatchIfEqualTo(
      Environment::Default().timeout_per_batch);

  if (size_t j = absl::GetFlag(FLAGS_j)) {
    env_from_flags.total_shards = j;
    env_from_flags.num_threads = j;
    env_from_flags.my_shard_index = 0;
  }
  CHECK_GE(env_from_flags.total_shards, 1);
  CHECK_GE(env_from_flags.batch_size, 1);
  CHECK_GE(env_from_flags.num_threads, 1);
  CHECK_LE(env_from_flags.num_threads, env_from_flags.total_shards);
  CHECK_LE(env_from_flags.my_shard_index + env_from_flags.num_threads,
           env_from_flags.total_shards)
      << VV(env_from_flags.my_shard_index) << VV(env_from_flags.num_threads);

  if (!argv.empty()) {
    env_from_flags.exec_name = argv[0];
    for (size_t i = 1; i < argv.size(); ++i) {
      env_from_flags.args.emplace_back(argv[i]);
    }
  }

  if (!env_from_flags.clang_coverage_binary.empty())
    env_from_flags.extra_binaries.push_back(
        env_from_flags.clang_coverage_binary);

  if (absl::StrContains(env_from_flags.binary, "@@")) {
    LOG(INFO) << "@@ detected; running in standalone mode with batch_size=1";
    env_from_flags.has_input_wildcards = true;
    env_from_flags.batch_size = 1;
    // TODO(kcc): do we need to check if extra_binaries have @@?
  }

  env_from_flags.ReadKnobsFileIfSpecified();
  return env_from_flags;
}

}  // namespace fuzztest::internal
