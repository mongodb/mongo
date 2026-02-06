#include "./fuzztest/init_fuzztest.h"

#if defined(__linux__)
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "absl/algorithm/container.h"
#include "absl/base/no_destructor.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/flags/reflection.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "absl/time/time.h"
#include "./fuzztest/internal/configuration.h"
#include "./fuzztest/internal/flag_name.h"
#include "./fuzztest/internal/googletest_adaptor.h"
#include "./fuzztest/internal/io.h"
#include "./fuzztest/internal/logging.h"
#include "./fuzztest/internal/registry.h"
#include "./fuzztest/internal/runtime.h"

#define FUZZTEST_DEFINE_FLAG(type, name, default_value, description) \
  ABSL_FLAG(type, FUZZTEST_FLAG_NAME(name), default_value, description)

FUZZTEST_DEFINE_FLAG(
    bool, list_fuzz_tests, false,
    "Prints (to stdout) the list of all available FUZZ_TEST-s in the "
    "binary and exits. I.e., prints the test names that can be run with "
    "the flag `--" FUZZTEST_FLAG_PREFIX "fuzz=<test name>`.")
    .OnUpdate([]() {
      fuzztest::internal::SetFuzzTestListingModeValidatorForGoogleTest(
          absl::GetFlag(FUZZTEST_FLAG(list_fuzz_tests)));
    });

static constexpr absl::string_view kUnspecified = "<unspecified>";

FUZZTEST_DEFINE_FLAG(
    std::string, fuzz, std::string(kUnspecified),
    "Runs a single FUZZ_TEST in continuous fuzzing mode. E.g., "
    "`--" FUZZTEST_FLAG_PREFIX
    "fuzz=MySuite.MyFuzzTest` runs the given FUZZ_TEST in fuzzing mode. You "
    "can also provide just a part of the name, e.g., "
    "`--" FUZZTEST_FLAG_PREFIX
    "fuzz=MyFuzz`, if it matches only a single FUZZ_TEST. If you have only one "
    "fuzz test in your binary, you can also use `--" FUZZTEST_FLAG_PREFIX
    "fuzz=` to run it in fuzzing mode (i.e., by setting the flag to empty "
    "string). In fuzzing mode the selected test runs until a bug is found or "
    "until manually stopped. Fuzzing mode uses coverage feedback to "
    "iteratively build up a corpus of inputs that maximize coverage and to "
    "reach deep bugs. Note that the binary must be compiled with "
    "`--config=fuzztest` for this to work, as it needs coverage "
    "instrumentation.");

FUZZTEST_DEFINE_FLAG(
    absl::Duration, fuzz_for, absl::InfiniteDuration(),
    "Runs all fuzz tests in fuzzing mode up to the specified duration. Can "
    "be combined with --" FUZZTEST_FLAG_PREFIX
    "fuzz to select a single fuzz tests, or with --" GTEST_FLAG_PREFIX_
    "filter to select a subset of fuzz tests. It's recommended to run the "
    "tests in the binary in parallel (e.g., "
    "https://bazel.build/reference/test-encyclopedia#test-sharding). This flag "
    "can be useful for setting up periodic fuzzing in CI. The specified "
    "duration is the maximum time used for fuzzing a single FUZZ_TEST or all "
    "FUZZ_TESTs in the binary, based on the value of --" FUZZTEST_FLAG_PREFIX
    "time_budget_type.");

FUZZTEST_DEFINE_FLAG(
    std::string, corpus_database,
    "~/.cache/fuzztest",
    "The directory containing all corpora for all fuzz tests in the project. "
    "For each test binary, there's a corresponding <binary_name> "
    "subdirectory in `corpus_database`, and  the <binary_name> directory has "
    "the following structure: (1) For each fuzz test `SuiteName.TestName` in "
    "the binary, there's a sub-directory with the name of that test "
    "('<binary_name>/SuiteName.TestName'). (2) For each fuzz test, there are "
    "three directories containing `regression`, `crashing`, and `coverage` "
    "directories. Files in the `regression` directory will always be used. "
    "Files in `crashing` directory will be used when "
    "--reproduce_findings_as_separate_tests flag is true. And finally, all "
    "files in `coverage` directory will be used when --replay_corpus flag is "
    "specified.");

FUZZTEST_DEFINE_FLAG(bool, reproduce_findings_as_separate_tests, false,
                     "When true, the selected tests replay all crashing inputs "
                     "in the corpus database as separate TEST-s.");

FUZZTEST_DEFINE_FLAG(
    std::string, replay_corpus, std::string(kUnspecified),
    "Runs a single FUZZ_TEST in corpus replay mode. E.g., "
    "`--" FUZZTEST_FLAG_PREFIX
    "replay_corpus=MySuite.MyFuzzTest` replays the corpus from the corpus "
    "database for the given FUZZ_TEST. You can also provide just a part of the "
    "name, e.g., `--" FUZZTEST_FLAG_PREFIX
    "replay_corpus=MyFuzz`, if it matches only a single FUZZ_TEST. If you have "
    "only one fuzz test in your binary, you can also use "
    "`--" FUZZTEST_FLAG_PREFIX
    "replay_corpus=` to run it in the corpus replay mode (i.e., by setting the "
    "flag to empty string). In corpus replay mode the selected test replays "
    "the inputs that exercise unique coverage (corpus) from the corpus "
    "database. Note that this does not include crashing inputs (counterexample "
    "findings). Replaying coverage (non-crashing) inputs is useful for "
    "measuring the coverage of the corpus built up during previously ran "
    "fuzzing sessions, or to catch newly introduced regressions at presubmit "
    "time in CI.");

FUZZTEST_DEFINE_FLAG(
    absl::Duration, replay_corpus_for, absl::ZeroDuration(),
    "Runs all fuzz tests in corpus replay mode up to the specified duration. "
    "Can be combined with --" FUZZTEST_FLAG_PREFIX
    "fuzz to select a single fuzz test, or with --" GTEST_FLAG_PREFIX_
    "filter to select a subset of fuzz tests. To replay the entire corpus for "
    "each FUZZ_TEST in the binary, set this flag to `inf` (infinite time "
    "budget). It's recommended to run the tests in the binary in parallel "
    "(e.g., https://bazel.build/reference/test-encyclopedia#test-sharding). "
    "The specified duration is the maximum time used for replaying the corpus "
    "for a single FUZZ_TEST or all FUZZ_TESTs in the binary, based on the "
    "value of --" FUZZTEST_FLAG_PREFIX "time_budget_type.");

FUZZTEST_DEFINE_FLAG(
    fuzztest::internal::TimeBudgetType, time_budget_type,
    fuzztest::internal::TimeBudgetType::kPerTest,
    "Determines whether the time budget specified by --" FUZZTEST_FLAG_PREFIX
    "fuzz_for or --" FUZZTEST_FLAG_PREFIX
    "replay_corpus_for is for each FUZZ_TEST or for all 'N' running FUZZ_TESTs "
    ". In the latter case, each FUZZ_TEST will run for at most (1/N)th of the "
    "time budget.");

FUZZTEST_DEFINE_FLAG(
    std::optional<std::string>, execution_id, std::nullopt,
    "If set, will resume or skip running on the corpus database for tests that "
    "are previously run with the same execution ID.");

FUZZTEST_DEFINE_FLAG(
    size_t, stack_limit_kb, 128,
    "The soft limit of the stack size in kibibytes to abort when "
    "the limit is exceeded. 0 indicates no limit.");

FUZZTEST_DEFINE_FLAG(size_t, rss_limit_mb, 0,
                     "The soft limit of the RSS size in mebibytes to abort "
                     "when the limit is exceeded. 0 indicates no limit.");

FUZZTEST_DEFINE_FLAG(
    absl::Duration, time_limit_per_input, absl::InfiniteDuration(),
    "The time limit of the property-function: A timeout bug will be reported "
    "for an input if the execution of the property-function with the input "
    "takes longer than this time limit.");

FUZZTEST_DEFINE_FLAG(std::optional<size_t>, jobs, std::nullopt,
                     "The number of fuzzing jobs to run in parallel. If "
                     "unspecified, the number of jobs is 1.");

FUZZTEST_DEFINE_FLAG(
    bool, print_subprocess_log, false,
    "If set, print the log of the subprocesses spawned by FuzzTest.");

// Internal flags - not part of the user interface.
//
// These flags are meant to be set only by the parent controller process for its
// child processes.
//
// TODO(b/406001082): Remove these flags once they are no longer needed.

FUZZTEST_DEFINE_FLAG(
    std::optional<std::string>, internal_centipede_command, std::nullopt,
    "If set, run the Centipede command in separate processes as the fuzzing "
    "engine. This flag is for internal use only.");

FUZZTEST_DEFINE_FLAG(
    std::optional<std::string>, internal_override_fuzz_test, std::nullopt,
    "Internal-only flag - do not use directly. If set, only perform operations "
    "for the exact fuzz test regardless of other flags.")
    .OnUpdate([] {
      FUZZTEST_INTERNAL_CHECK_PRECONDITION(
          !absl::GetFlag(FUZZTEST_FLAG(internal_override_fuzz_test))
                  .has_value() ||
              std::getenv("CENTIPEDE_RUNNER_FLAGS") != nullptr,
          "must not set --" FUZZTEST_FLAG_PREFIX
          "internal_override_fuzz_test directly");
    });

FUZZTEST_DEFINE_FLAG(
    absl::Duration, internal_override_total_time_limit,
    absl::InfiniteDuration(),
    "Internal-only flag - do not use directly. If --" FUZZTEST_FLAG_PREFIX
    "internal_override_fuzz_test is set, override the time limit set by "
    "--" FUZZTEST_FLAG_PREFIX "fuzz_for / --" FUZZTEST_FLAG_PREFIX
    "replay_corpus_for with --time_budget_type set to total.")
    .OnUpdate([] {
      FUZZTEST_INTERNAL_CHECK_PRECONDITION(
          absl::GetFlag(FUZZTEST_FLAG(internal_override_total_time_limit)) ==
                  absl::InfiniteDuration() ||
              std::getenv("CENTIPEDE_RUNNER_FLAGS") != nullptr,
          "must not set --" FUZZTEST_FLAG_PREFIX
          "internal_override_total_time_limit directly");
    });

FUZZTEST_DEFINE_FLAG(std::optional<std::string>,
                     internal_crashing_input_to_reproduce, std::nullopt,
                     "Internal-only flag - do not use directly. If both this "
                     "and --" FUZZTEST_FLAG_PREFIX
                     "internal_override_fuzz_test are set, replay "
                     "the input in the corpus database with the specified ID.")
    .OnUpdate([] {
      FUZZTEST_INTERNAL_CHECK_PRECONDITION(
          !absl::GetFlag(FUZZTEST_FLAG(internal_crashing_input_to_reproduce))
                  .has_value() ||
              std::getenv("CENTIPEDE_RUNNER_FLAGS") != nullptr,
          "must not set --" FUZZTEST_FLAG_PREFIX
          "internal_crashing_input_to_reproduce directly");
    });

namespace fuzztest {

std::vector<std::string> ListRegisteredTests() {
  std::vector<std::string> result;
  internal::ForEachTest(
      [&](const auto& test) { result.push_back(test.full_name()); });
  return result;
}

std::string GetMatchingFuzzTestOrExit(std::string_view name) {
  const std::string partial_name(name);
  const std::vector<std::string> full_names = ListRegisteredTests();
  std::vector<const std::string*> matches;
  for (const std::string& full_name : full_names) {
    if (absl::StrContains(full_name, partial_name)) {
      if (full_name == partial_name) {
        // In case of an exact match, we end the search and use it. This is to
        // handle the case when we want to select `MySuite.MyTest`, but the
        // binary has both `MySuite.MyTest` and `MySuite.MyTestX`.
        return full_name;
      } else {
        matches.push_back(&full_name);
      }
    }
  }

  if (matches.empty()) {
    absl::FPrintF(stderr, "\n\nNo FUZZ_TEST matches the name: %s\n\n",
                  partial_name);
    absl::FPrintF(stderr, "Valid tests:\n");
    for (const std::string& full_name : full_names) {
      absl::FPrintF(stderr, " %s\n", full_name);
    }
    exit(1);
  } else if (matches.size() > 1) {
    absl::FPrintF(stderr, "\n\nMultiple FUZZ_TESTs match the name: %s\n\n",
                  partial_name);
    absl::FPrintF(stderr, "Please select one. Matching tests:\n");
    for (const std::string* full_name : matches) {
      absl::FPrintF(stderr, " %s\n", *full_name);
    }
    exit(1);
  }
  return *matches[0];
}

namespace {

std::optional<absl::Duration> GetFuzzingTime() {
  absl::Duration fuzz_time_limit = absl::GetFlag(FUZZTEST_FLAG(fuzz_for));
  if (fuzz_time_limit <= absl::ZeroDuration()) {
    fuzz_time_limit = absl::InfiniteDuration();
  }
  if (absl::GetFlag(FUZZTEST_FLAG(fuzz)) == kUnspecified &&
      fuzz_time_limit == absl::InfiniteDuration()) {
    return std::nullopt;
  }
  return fuzz_time_limit;
}

std::optional<absl::Duration> GetReplayCorpusTime() {
  absl::Duration replay_corpus_time_limit =
      absl::GetFlag(FUZZTEST_FLAG(replay_corpus_for));
  if (absl::GetFlag(FUZZTEST_FLAG(replay_corpus)) == kUnspecified &&
      replay_corpus_time_limit <= absl::ZeroDuration()) {
    return std::nullopt;
  }
  if (absl::GetFlag(FUZZTEST_FLAG(replay_corpus)) != kUnspecified &&
      replay_corpus_time_limit <= absl::ZeroDuration()) {
    replay_corpus_time_limit = absl::InfiniteDuration();
  }
  return replay_corpus_time_limit;
}

internal::Configuration CreateConfigurationsFromFlags(
    absl::string_view binary_identifier) {
  const bool reproduce_findings_as_separate_tests =
      absl::GetFlag(FUZZTEST_FLAG(reproduce_findings_as_separate_tests));
  const std::optional<absl::Duration> fuzzing_time_limit = GetFuzzingTime();
  const std::optional<absl::Duration> replay_corpus_time_limit =
      GetReplayCorpusTime();
  const std::optional<std::string> override_fuzz_test =
      absl::GetFlag(FUZZTEST_FLAG(internal_override_fuzz_test));
  const bool replay_coverage_inputs =
      fuzzing_time_limit.has_value() || replay_corpus_time_limit.has_value();
  const absl::Duration time_limit =
      override_fuzz_test.has_value()
          ? absl::GetFlag(FUZZTEST_FLAG(internal_override_total_time_limit))
      : fuzzing_time_limit.has_value()       ? *fuzzing_time_limit
      : replay_corpus_time_limit.has_value() ? *replay_corpus_time_limit
                                             : absl::ZeroDuration();
  const internal::TimeBudgetType time_budget_type =
      override_fuzz_test.has_value()
          ? internal::TimeBudgetType::kTotal
          : absl::GetFlag(FUZZTEST_FLAG(time_budget_type));
  const std::optional<size_t> jobs = absl::GetFlag(FUZZTEST_FLAG(jobs));
  FUZZTEST_INTERNAL_CHECK(!jobs.has_value() || *jobs > 0, "If specified, --",
                          FUZZTEST_FLAG(jobs).Name(), " must be positive.");
  return internal::Configuration{
      absl::GetFlag(FUZZTEST_FLAG(corpus_database)),
      /*stats_root=*/"",
      /*workdir_root=*/"",
      std::string(binary_identifier),
      /*fuzz_tests=*/ListRegisteredTests(),
      /*fuzz_tests_in_current_shard=*/ListRegisteredTests(),
      reproduce_findings_as_separate_tests,
      replay_coverage_inputs,
      /*only_replay=*/
      replay_corpus_time_limit.has_value(),
      /*replay_in_single_process=*/false,
      absl::GetFlag(FUZZTEST_FLAG(execution_id)),
      absl::GetFlag(FUZZTEST_FLAG(print_subprocess_log)),
      /*stack_limit=*/absl::GetFlag(FUZZTEST_FLAG(stack_limit_kb)) * 1024,
      /*rss_limit=*/absl::GetFlag(FUZZTEST_FLAG(rss_limit_mb)) * 1024 * 1024,
      absl::GetFlag(FUZZTEST_FLAG(time_limit_per_input)),
      time_limit,
      time_budget_type,
      jobs.value_or(0),
      absl::GetFlag(FUZZTEST_FLAG(internal_centipede_command)),
      absl::GetFlag(FUZZTEST_FLAG(internal_crashing_input_to_reproduce)),
  };
}
}  // namespace

void RunSpecifiedFuzzTest(std::string_view name, std::string_view binary_id) {
  const std::string matching_fuzz_test = GetMatchingFuzzTestOrExit(name);
  internal::Configuration configuration =
      CreateConfigurationsFromFlags({binary_id.data(), binary_id.size()});
  internal::ForEachTest([&](auto& test) {
    // TODO(b/301965259): Properly initialize the configuration.
    if (test.full_name() == matching_fuzz_test) {
      std::exit(test.make()->RunInFuzzingMode(/*argc=*/nullptr,
                                              /*argv=*/nullptr, configuration));
    }
  });
}

void InitFuzzTest(int* argc, char*** argv, std::string_view binary_id) {
  auto& runtime = internal::Runtime::instance();
  const bool is_listing = absl::GetFlag(FUZZTEST_FLAG(list_fuzz_tests));
  if (is_listing) {
    for (const auto& name : ListRegisteredTests()) {
      std::cout << "[*] Fuzz test: " << name << '\n';
    }
    std::cout << std::flush;
    std::exit(0);
  }
  std::optional<absl::Duration> fuzzing_time_limit = GetFuzzingTime();
  std::optional<absl::Duration> replay_corpus_time_limit =
      GetReplayCorpusTime();
  FUZZTEST_INTERNAL_CHECK(
      !fuzzing_time_limit || !replay_corpus_time_limit,
      "Cannot run in fuzzing and corpus replay mode at the same time.");
  const auto test_to_fuzz = absl::GetFlag(FUZZTEST_FLAG(fuzz));
  const auto test_to_replay_corpus =
      absl::GetFlag(FUZZTEST_FLAG(replay_corpus));
  const auto specified_test = []() -> std::optional<std::string> {
    if (auto internal_selected_test =
            absl::GetFlag(FUZZTEST_FLAG(internal_override_fuzz_test));
        internal_selected_test.has_value()) {
      return internal_selected_test;
    }
    if (auto test_to_fuzz = absl::GetFlag(FUZZTEST_FLAG(fuzz));
        test_to_fuzz != kUnspecified) {
      return test_to_fuzz;
    }
    if (auto test_to_replay_corpus =
            absl::GetFlag(FUZZTEST_FLAG(replay_corpus));
        test_to_replay_corpus != kUnspecified) {
      return test_to_replay_corpus;
    }
    return std::nullopt;
  }();
  if (specified_test.has_value()) {
    const std::string matching_fuzz_test =
        GetMatchingFuzzTestOrExit(*specified_test);
    // Delegate the test to GoogleTest.
    GTEST_FLAG_SET(filter, matching_fuzz_test);
  }

  std::string derived_binary_id =
      binary_id.empty() ? std::string(internal::Basename(*argv[0]))
                        : std::string(binary_id);
  std::optional<std::string> reproduction_command_template;
  internal::Configuration configuration =
      CreateConfigurationsFromFlags(derived_binary_id);
  configuration.reproduction_command_template = reproduction_command_template;
  internal::RegisterFuzzTestsAsGoogleTests(argc, argv, configuration);

  const bool is_fuzzing_or_replaying =
      (fuzzing_time_limit || replay_corpus_time_limit);
  if (is_fuzzing_or_replaying && !specified_test.has_value()) {
    absl::flat_hash_set<std::string> fuzz_tests = {
        configuration.fuzz_tests.begin(), configuration.fuzz_tests.end()};
    std::vector<std::string> non_fuzz_tests;
    for (const auto* test : internal::GetRegisteredTests()) {
      const std::string test_name =
          absl::StrCat(test->test_suite_name(), ".", test->name());
      if (!fuzz_tests.contains(test_name)) {
        non_fuzz_tests.push_back(test_name);
      }
    }
    if (!non_fuzz_tests.empty()) {
      // Run only the fuzz tests, and not the unit tests.
      // TODO: b/340232436 -- This is needed because we currently rely on a fuzz
      // test being the first test to run so that Centipede can get the
      // serialized configuration from the binary.
      std::string filter = absl::StrCat(
          GTEST_FLAG_GET(filter),
          // When the filter already includes the negative patterns, append to
          // the negative patterns.
          absl::StrContains(GTEST_FLAG_GET(filter), '-') ? ":" : "-",
          absl::StrJoin(non_fuzz_tests, ":"));
      GTEST_FLAG_SET(filter, filter);
    }
  }
  const RunMode run_mode =
      fuzzing_time_limit.has_value() ? RunMode::kFuzz : RunMode::kUnitTest;
  // TODO(b/307513669): Use the Configuration class instead of Runtime.
  runtime.SetRunMode(run_mode);
}

void ParseAbslFlags(int argc, char** argv) {
  std::vector<char*> positional_args;
  std::vector<absl::UnrecognizedFlag> unrecognized_flags;
  absl::ParseAbseilFlagsOnly(argc, argv, positional_args, unrecognized_flags);
}

}  // namespace fuzztest
