// Copyright 2022 The Centipede Authors.
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

#include "./centipede/environment.h"

#include <cstddef>
#include <string_view>

#include "gtest/gtest.h"
#include "absl/log/check.h"
#include "absl/time/time.h"
#include "./fuzztest/internal/configuration.h"

namespace fuzztest::internal {

TEST(Environment, UpdateForExperiment) {
  Environment env;
  env.num_threads = 12;
  env.experiment = "use_cmp_features=false,true:path_level=10,20,30";

  auto Experiment = [&](size_t shard_index, bool val1, size_t val2,
                        std::string_view experiment_name,
                        std::string_view experiment_flags) {
    env.my_shard_index = shard_index;
    env.UpdateForExperiment();
    EXPECT_EQ(env.load_other_shard_frequency, 0);
    EXPECT_EQ(env.use_cmp_features, val1);
    EXPECT_EQ(env.path_level, val2);
    EXPECT_EQ(env.experiment_name, experiment_name);
    EXPECT_EQ(env.experiment_flags, experiment_flags);
  };

  Experiment(0, false, 10, "E00", "use_cmp_features=false:path_level=10:");
  Experiment(1, false, 20, "E01", "use_cmp_features=false:path_level=20:");
  Experiment(2, false, 30, "E02", "use_cmp_features=false:path_level=30:");
  Experiment(3, true, 10, "E10", "use_cmp_features=true:path_level=10:");
  Experiment(4, true, 20, "E11", "use_cmp_features=true:path_level=20:");
  Experiment(5, true, 30, "E12", "use_cmp_features=true:path_level=30:");
  Experiment(6, false, 10, "E00", "use_cmp_features=false:path_level=10:");
  Experiment(7, false, 20, "E01", "use_cmp_features=false:path_level=20:");
  Experiment(8, false, 30, "E02", "use_cmp_features=false:path_level=30:");
  Experiment(9, true, 10, "E10", "use_cmp_features=true:path_level=10:");
  Experiment(10, true, 20, "E11", "use_cmp_features=true:path_level=20:");
  Experiment(11, true, 30, "E12", "use_cmp_features=true:path_level=30:");
}

TEST(Environment, UpdatesNumberOfShardsAndThreadsFromTargetConfigJobs) {
  Environment env;
  env.total_shards = 20;
  env.my_shard_index = 10;
  env.num_threads = 5;
  fuzztest::internal::Configuration config;
  config.jobs = 10;
  env.UpdateWithTargetConfig(config);
  EXPECT_EQ(env.j, 10);
  EXPECT_EQ(env.total_shards, 10);
  EXPECT_EQ(env.my_shard_index, 0);
  EXPECT_EQ(env.num_threads, 10);
}

TEST(Environment, DiesOnInconsistentJAndTargetConfigJobs) {
  Environment env;
  env.j = 10;
  fuzztest::internal::Configuration config;
  config.jobs = 20;
  EXPECT_DEATH(env.UpdateWithTargetConfig(config),
               "Value for --j is inconsistent with the value for jobs in the "
               "target binary");
}

TEST(Environment, UpdatesTimeoutPerBatchFromTimeoutPerInputAndBatchSize) {
  Environment env;
  env.batch_size = 1000;
  env.timeout_per_input = 100;
  env.timeout_per_batch = 0;
  env.UpdateTimeoutPerBatchIfEqualTo(0);
  EXPECT_GT(env.timeout_per_batch, 0);

  env.timeout_per_batch = 123;
  env.UpdateTimeoutPerBatchIfEqualTo(0);
  EXPECT_EQ(env.timeout_per_batch, 123);
}

TEST(Environment,
     UpdatesTimeoutPerInputFromFiniteTargetConfigTimeLimitPerInput) {
  Environment env;
  env.timeout_per_input = Environment::Default().timeout_per_input;
  fuzztest::internal::Configuration config;
  config.time_limit_per_input = absl::Seconds(456);
  env.UpdateWithTargetConfig(config);
  EXPECT_EQ(env.timeout_per_input, 456);
}

TEST(Environment,
     UpdatesTimeoutPerInputFromInfiniteTargetConfigTimeLimitPerInput) {
  Environment env;
  env.timeout_per_input = Environment::Default().timeout_per_input;
  fuzztest::internal::Configuration config;
  config.time_limit_per_input = absl::InfiniteDuration();
  env.UpdateWithTargetConfig(config);
  EXPECT_EQ(env.timeout_per_input, 0);
}

TEST(Environment,
     DiesOnInconsistentTimeoutPerInputAndTargetConfigTimeLimitPerInput) {
  Environment env;
  env.timeout_per_input = 123;
  fuzztest::internal::Configuration config;
  config.time_limit_per_input = absl::Seconds(456);
  EXPECT_DEATH(
      env.UpdateWithTargetConfig(config),
      "Value for --timeout_per_input is inconsistent with the value for "
      "time_limit_per_input in the target binary");
}

TEST(Environment,
     UpdatesTimeoutPerBatchFromFiniteTargetConfigTimeLimitPerInput) {
  Environment env;
  env.timeout_per_input = Environment::Default().timeout_per_input;
  env.UpdateTimeoutPerBatchIfEqualTo(Environment::Default().timeout_per_batch);
  const size_t autocomputed_timeout_per_batch = env.timeout_per_batch;
  fuzztest::internal::Configuration config;
  config.time_limit_per_input = absl::Seconds(456);
  env.UpdateWithTargetConfig(config);
  EXPECT_NE(env.timeout_per_batch, autocomputed_timeout_per_batch);
}

TEST(Environment,
     UpdatesTimeoutPerBatchFromInfiniteTargetConfigTimeLimitPerInput) {
  Environment env;
  env.timeout_per_input = Environment::Default().timeout_per_input;
  env.UpdateTimeoutPerBatchIfEqualTo(Environment::Default().timeout_per_batch);
  fuzztest::internal::Configuration config;
  config.time_limit_per_input = absl::InfiniteDuration();
  env.UpdateWithTargetConfig(config);
  EXPECT_EQ(env.timeout_per_batch, 0);
}

TEST(Environment, UpdatesTimeoutPerBatchFromTargetConfigTimeLimit) {
  Environment env;
  fuzztest::internal::Configuration config;
  config.time_limit = absl::Seconds(123);
  config.time_budget_type = fuzztest::internal::TimeBudgetType::kPerTest;
  CHECK(config.GetTimeLimitPerTest() == absl::Seconds(123));
  env.UpdateWithTargetConfig(config);
  EXPECT_EQ(env.timeout_per_batch, 123)
      << "`timeout_per_batch` should be set to the test time limit when it was "
         "previously unset";

  env.timeout_per_batch = 456;
  env.UpdateWithTargetConfig(config);
  EXPECT_EQ(env.timeout_per_batch, 123)
      << "`timeout_per_batch` should be set to test time limit when it is "
         "shorter than the previous value";

  env.timeout_per_batch = 56;
  env.UpdateWithTargetConfig(config);
  EXPECT_EQ(env.timeout_per_batch, 56)
      << "`timeout_per_batch` should not be updated with the test time limit "
         "when it is longer than the previous value";
}

TEST(Environment, UpdatesRssLimitMbFromTargetConfigRssLimit) {
  Environment env;
  env.rss_limit_mb = Environment::Default().rss_limit_mb;
  fuzztest::internal::Configuration config;
  config.rss_limit = 5UL * 1024 * 1024 * 1024;
  env.UpdateWithTargetConfig(config);
  EXPECT_EQ(env.rss_limit_mb, 5 * 1024);
}

TEST(Environment, DiesOnInconsistentRssLimitMbAndTargetConfigRssLimit) {
  Environment env;
  env.rss_limit_mb = 123;
  fuzztest::internal::Configuration config;
  config.rss_limit = 5UL * 1024 * 1024 * 1024;
  EXPECT_DEATH(
      env.UpdateWithTargetConfig(config),
      "Value for --rss_limit_mb is inconsistent with the value for rss_limit "
      "in the target binary");
}

TEST(Environment, UpdatesStackLimitKbFromTargetConfigStackLimit) {
  Environment env;
  env.stack_limit_kb = Environment::Default().stack_limit_kb;
  fuzztest::internal::Configuration config;
  config.stack_limit = 5UL * 1024;
  env.UpdateWithTargetConfig(config);
  EXPECT_EQ(env.stack_limit_kb, 5);
}

TEST(Environment, DiesOnInconsistentStackLimitKbAndTargetConfigStackLimit) {
  Environment env;
  env.stack_limit_kb = 123;
  fuzztest::internal::Configuration config;
  config.stack_limit = 5UL * 1024;
  EXPECT_DEATH(env.UpdateWithTargetConfig(config),
               "Value for --stack_limit_kb is inconsistent with the value for "
               "stack_limit in the target binary");
}

TEST(Environment, UpdatesReplayOnlyConfiguration) {
  Environment env;
  fuzztest::internal::Configuration config;
  config.only_replay = true;
  env.UpdateWithTargetConfig(config);
  EXPECT_TRUE(env.load_shards_only);
  EXPECT_FALSE(env.populate_binary_info);
}

}  // namespace fuzztest::internal
