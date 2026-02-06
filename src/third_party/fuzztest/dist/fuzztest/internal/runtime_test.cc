// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "./fuzztest/internal/runtime.h"

#include <string>
#include <tuple>

#include "gtest/gtest.h"
#include "absl/strings/match.h"
#include "absl/time/time.h"
#include "./fuzztest/domain_core.h"
#include "./fuzztest/internal/configuration.h"
#include "./fuzztest/internal/test_protobuf.pb.h"

namespace fuzztest::internal {
namespace {

TEST(OnFailureTest, Output) {
  auto& runtime = Runtime::instance();
  const auto get_failure = [&] {
    std::string s;
    runtime.PrintReport(&s);
    return s;
  };
  // Disabled by default.
  EXPECT_EQ(get_failure(), "");

  FuzzTest test({"SUITE_NAME", "TEST_NAME", "FILE", 123}, nullptr);
  Configuration configuration;
  std::tuple args(17, std::string("ABC"));
  const RuntimeStats stats = {absl::FromUnixNanos(0), 1, 2, 3, 4, 5};
  runtime.EnableReporter(&stats, [] { return absl::FromUnixNanos(1979); });
  runtime.SetRunMode(RunMode::kFuzz);
  UntypedDomain domain = TupleOf(Arbitrary<int>(), Arbitrary<std::string>());
  GenericDomainCorpusType generic_args(
      std::in_place_type<std::tuple<int, std::string>>, args);
  Runtime::Args debug_args{generic_args, domain};
  runtime.SetCurrentTest(&test, &configuration);
  runtime.SetCurrentArgs(&debug_args);
  const std::string report = get_failure();

  EXPECT_TRUE(absl::StartsWith(report, R"(
=================================================================
=== Fuzzing stats

Elapsed time: 1.979us
Total runs: 1
Edges covered: 2
Total edges: 3
Corpus size: 4
Max stack used: 5

=================================================================
=== BUG FOUND!

FILE:123: Counterexample found for SUITE_NAME.TEST_NAME.
The test fails with input:
argument 0: 17
argument 1: "ABC"
)"));

  EXPECT_TRUE(absl::StrContains(report, R"(
=================================================================
=== Regression test draft

TEST(SUITE_NAME, TEST_NAMERegression) {
  TEST_NAME(
    17,
    "ABC"
  );
}

Please note that the code generated above is best effort and is intended
to be used as a draft regression test.
For reproducing findings please rely on file based reproduction.

=================================================================
)"));

  runtime.DisableReporter();
  EXPECT_EQ(get_failure(), "");
}

}  // namespace
}  // namespace fuzztest::internal
