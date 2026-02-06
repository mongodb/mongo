#include "./fuzztest/internal/configuration.h"

#include <optional>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/statusor.h"
#include "absl/time/time.h"

namespace fuzztest::internal {
namespace {

// Compares the fields of `Configuration` that are relevant for serialization.
MATCHER_P(IsOkAndEquals, config, "") {
  const absl::StatusOr<Configuration>& other = arg;
  return other.ok() && config.corpus_database == other->corpus_database &&
         config.stats_root == other->stats_root &&
         config.workdir_root == other->workdir_root &&
         config.binary_identifier == other->binary_identifier &&
         config.fuzz_tests == other->fuzz_tests &&
         config.fuzz_tests_in_current_shard ==
             other->fuzz_tests_in_current_shard &&
         config.reproduce_findings_as_separate_tests ==
             other->reproduce_findings_as_separate_tests &&
         config.replay_coverage_inputs == other->replay_coverage_inputs &&
         config.only_replay == other->only_replay &&
         config.replay_in_single_process == other->replay_in_single_process &&
         config.execution_id == other->execution_id &&
         config.print_subprocess_log == other->print_subprocess_log &&
         config.stack_limit == other->stack_limit &&
         config.rss_limit == other->rss_limit &&
         config.time_limit_per_input == other->time_limit_per_input &&
         config.time_limit == other->time_limit &&
         config.time_budget_type == other->time_budget_type &&
         config.jobs == other->jobs &&
         config.centipede_command == other->centipede_command &&
         config.crashing_input_to_reproduce ==
             other->crashing_input_to_reproduce &&
         config.reproduction_command_template ==
             other->reproduction_command_template;
}

TEST(ConfigurationTest,
     DeserializeYieldsSerializedConfigurationWithoutOptionalValues) {
  Configuration configuration{"corpus_database",
                              "stats_root",
                              "workdir_root",
                              "binary_identifier",
                              /*fuzz_tests=*/{},
                              /*fuzz_tests_in_current_shard=*/{},
                              /*reproduce_findings_as_separate_tests=*/true,
                              /*replay_coverage_inputs=*/true,
                              /*only_replay=*/true,
                              /*replay_in_single_process=*/true,
                              "execution_id",
                              /*print_subprocess_log=*/true,
                              /*stack_limit=*/100,
                              /*rss_limit=*/200,
                              /*time_limit_per_input=*/absl::Seconds(42),
                              /*time_limit=*/absl::Minutes(42),
                              /*time_budget_type=*/TimeBudgetType::kPerTest,
                              /*jobs=*/1,
                              /*centipede_command=*/std::nullopt,
                              /*crashing_input_to_reproduce=*/std::nullopt,
                              /*reproduction_command_template=*/std::nullopt};

  EXPECT_THAT(Configuration::Deserialize(configuration.Serialize()),
              IsOkAndEquals(configuration));
}

TEST(ConfigurationTest,
     DeserializeYieldsSerializedConfigurationWithOptionalValues) {
  Configuration configuration{"corpus_database",
                              "stats_root",
                              "workdir_root",
                              "binary_identifier",
                              {"FuzzTest1", "FuzzTest2"},
                              {"FuzzTest1"},
                              /*reproduce_findings_as_separate_tests=*/true,
                              /*replay_coverage_inputs=*/true,
                              /*only_replay=*/true,
                              /*replay_in_single_process=*/true,
                              "execution_id",
                              /*print_subprocess_log=*/true,
                              /*stack_limit=*/100,
                              /*rss_limit=*/200,
                              /*time_limit_per_input=*/absl::Seconds(42),
                              /*time_limit=*/absl::Minutes(42),
                              /*time_budget_type=*/TimeBudgetType::kPerTest,
                              /*jobs=*/1,
                              "centipede_command",
                              "crashing_input_to_reproduce",
                              "reproduction_command_template"};

  EXPECT_THAT(Configuration::Deserialize(configuration.Serialize()),
              IsOkAndEquals(configuration));
}

TEST(ConfigurationTest, DeserializeFailsOnNonsenseInput) {
  EXPECT_FALSE(
      Configuration::Deserialize("Not a serialized configuration").ok());
}

}  // namespace
}  // namespace fuzztest::internal
