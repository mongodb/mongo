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

#include "./fuzztest/internal/fixture_driver.h"

#include <tuple>
#include <utility>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/functional/any_invocable.h"
#include "absl/types/span.h"
#include "./fuzztest/domain_core.h"
#include "./fuzztest/internal/any.h"
#include "./fuzztest/internal/logging.h"
#include "./fuzztest/internal/registration.h"

namespace fuzztest::internal {
namespace {

using ::testing::UnorderedElementsAre;

struct CallCountFixture {
  void IncrementCallCount(int n) { call_count += n; }
  inline static int call_count;
};

using IncrementCallCountFunc = decltype(&CallCountFixture::IncrementCallCount);
using CallCountRegBase =
    DefaultRegistrationBase<CallCountFixture, IncrementCallCountFunc>;

template <typename... T>
MoveOnlyAny MakeArgs(T... t) {
  return MoveOnlyAny(std::in_place_type<std::tuple<T...>>, std::tuple(t...));
}

TEST(FixtureDriverTest, PropagatesCallToTargetFunction) {
  FixtureDriverImpl<Domain<std::tuple<int>>, CallCountFixture,
                    IncrementCallCountFunc, void*>
      fixture_driver(&CallCountFixture::IncrementCallCount,
                     Arbitrary<std::tuple<int>>(), {}, nullptr);

  CallCountFixture::call_count = 0;

  fixture_driver.RunFuzzTest([&] {
    fixture_driver.RunFuzzTestIteration(
        [&] { fixture_driver.Test(MakeArgs(7)); });
  });

  EXPECT_EQ(CallCountFixture::call_count, 7);
}

TEST(FixtureDriverTest, ReusesSameFixtureObjectDuringFuzzTest) {
  FixtureDriverImpl<Domain<std::tuple<int>>, CallCountFixture,
                    IncrementCallCountFunc, void*>
      fixture_driver(&CallCountFixture::IncrementCallCount,
                     Arbitrary<std::tuple<int>>(), {}, nullptr);

  CallCountFixture::call_count = 0;

  fixture_driver.RunFuzzTest([&] {
    fixture_driver.RunFuzzTestIteration(
        [&] { fixture_driver.Test(MakeArgs(3)); });
    fixture_driver.RunFuzzTestIteration(
        [&] { fixture_driver.Test(MakeArgs(3)); });
    fixture_driver.RunFuzzTestIteration(
        [&] { fixture_driver.Test(MakeArgs(4)); });
  });
  EXPECT_EQ(CallCountFixture::call_count, 10);
}

struct DerivedCallCountFixture : CallCountFixture {};

TEST(FixtureDriverTest, PropagatesCallToTargetFunctionOnBaseFixture) {
  FixtureDriverImpl<Domain<std::tuple<int>>, DerivedCallCountFixture,
                    IncrementCallCountFunc, void*>
      fixture_driver(&DerivedCallCountFixture::IncrementCallCount,
                     Arbitrary<std::tuple<int>>(), {}, nullptr);

  CallCountFixture::call_count = 0;

  fixture_driver.RunFuzzTest([&] {
    fixture_driver.RunFuzzTestIteration(
        [&] { fixture_driver.Test(MakeArgs(3)); });
  });

  EXPECT_EQ(CallCountFixture::call_count, 3);
}

struct LifecycleRecordingFixture {
  LifecycleRecordingFixture() { was_constructed = true; }
  ~LifecycleRecordingFixture() { was_destructed = true; }

  void NoOp() {}

  static void Reset() {
    was_constructed = false;
    was_destructed = false;
  }

  static bool was_constructed;
  static bool was_destructed;
};

bool LifecycleRecordingFixture::was_constructed = false;
bool LifecycleRecordingFixture::was_destructed = false;

TEST(FixtureDriverTest, FixtureGoesThroughCompleteLifecycle) {
  using NoOpFunc = decltype(&LifecycleRecordingFixture::NoOp);
  FixtureDriverImpl<Domain<std::tuple<>>, LifecycleRecordingFixture, NoOpFunc,
                    void*>
      fixture_driver(&LifecycleRecordingFixture::NoOp,
                     Arbitrary<std::tuple<>>(), {}, nullptr);

  LifecycleRecordingFixture::Reset();

  ASSERT_TRUE(!LifecycleRecordingFixture::was_constructed &&
              !LifecycleRecordingFixture::was_destructed);

  fixture_driver.RunFuzzTest([&] {
    fixture_driver.RunFuzzTestIteration(
        [&] { EXPECT_TRUE(LifecycleRecordingFixture::was_constructed); });
    EXPECT_TRUE(!LifecycleRecordingFixture::was_destructed);
  });

  EXPECT_TRUE(LifecycleRecordingFixture::was_destructed);
}

template <typename InstantiationType>
struct LifecycleRecordingFixtureWithExplicitSetUp : LifecycleRecordingFixture,
                                                    InstantiationType {
  ~LifecycleRecordingFixtureWithExplicitSetUp() override {
    LifecycleRecordingFixture::~LifecycleRecordingFixture();
  }

  void SetUp() override { was_set_up = true; }
  void TearDown() override { was_torn_down = true; }

  static void Reset() {
    LifecycleRecordingFixture::Reset();
    was_set_up = false;
    was_torn_down = false;
  }

  static bool was_set_up;
  static bool was_torn_down;
};

template <typename InstantiationType>
bool LifecycleRecordingFixtureWithExplicitSetUp<InstantiationType>::was_set_up =
    false;
template <typename InstantiationType>
bool LifecycleRecordingFixtureWithExplicitSetUp<
    InstantiationType>::was_torn_down = false;

TEST(FixtureDriverTest, PerIterationFixtureGoesThroughCompleteLifecycle) {
  using LifecycleRecordingPerIterationFixture =
      LifecycleRecordingFixtureWithExplicitSetUp<PerIterationFixture>;
  using NoOpFunc = decltype(&LifecycleRecordingPerIterationFixture::NoOp);
  FixtureDriverImpl<Domain<std::tuple<>>, LifecycleRecordingPerIterationFixture,
                    NoOpFunc, void*>
      fixture_driver(&LifecycleRecordingPerIterationFixture::NoOp,
                     Arbitrary<std::tuple<>>(), {}, nullptr);

  LifecycleRecordingPerIterationFixture::Reset();

  ASSERT_TRUE(!LifecycleRecordingPerIterationFixture::was_constructed &&
              !LifecycleRecordingPerIterationFixture::was_set_up &&
              !LifecycleRecordingPerIterationFixture::was_torn_down &&
              !LifecycleRecordingPerIterationFixture::was_destructed);

  fixture_driver.RunFuzzTest([&] {
    fixture_driver.RunFuzzTestIteration([&] {
      EXPECT_TRUE(LifecycleRecordingPerIterationFixture::was_constructed &&
                  LifecycleRecordingPerIterationFixture::was_set_up &&
                  !LifecycleRecordingPerIterationFixture::was_torn_down &&
                  !LifecycleRecordingPerIterationFixture::was_destructed);
    });
    EXPECT_TRUE(LifecycleRecordingPerIterationFixture::was_torn_down &&
                LifecycleRecordingPerIterationFixture::was_destructed);
  });
}

TEST(FixtureDriverTest, PerFuzzTestFixtureGoesThroughCompleteLifecycle) {
  using LifecycleRecordingPerFuzzTestFixture =
      LifecycleRecordingFixtureWithExplicitSetUp<PerFuzzTestFixture>;
  using NoOpFunc = decltype(&LifecycleRecordingPerFuzzTestFixture::NoOp);
  FixtureDriverImpl<Domain<std::tuple<>>, LifecycleRecordingPerFuzzTestFixture,
                    NoOpFunc, void*>
      fixture_driver(&LifecycleRecordingPerFuzzTestFixture::NoOp,
                     Arbitrary<std::tuple<>>(), {}, nullptr);
  LifecycleRecordingPerFuzzTestFixture::Reset();

  ASSERT_TRUE(!LifecycleRecordingPerFuzzTestFixture::was_constructed &&
              !LifecycleRecordingPerFuzzTestFixture::was_set_up &&
              !LifecycleRecordingPerFuzzTestFixture::was_torn_down &&
              !LifecycleRecordingPerFuzzTestFixture::was_destructed);

  fixture_driver.RunFuzzTest([&] {
    fixture_driver.RunFuzzTestIteration([&] {
      EXPECT_TRUE(LifecycleRecordingPerFuzzTestFixture::was_constructed &&
                  LifecycleRecordingPerFuzzTestFixture::was_set_up);
    });

    EXPECT_TRUE(!LifecycleRecordingPerFuzzTestFixture::was_torn_down &&
                !LifecycleRecordingPerFuzzTestFixture::was_destructed);
  });

  EXPECT_TRUE(LifecycleRecordingPerFuzzTestFixture::was_torn_down &&
              LifecycleRecordingPerFuzzTestFixture::was_destructed);
}

struct ExampleRunnerFixture : public FuzzTestRunnerFixture,
                              public IterationRunnerFixture {
  void FuzzTestRunner(absl::AnyInvocable<void() &&> run_test) {
    ++fuzz_test_runner_called;
    std::move(run_test)();
  }

  void FuzzTestIterationRunner(absl::AnyInvocable<void() &&> run_iteration) {
    ++fuzz_test_iteration_runner_called;
    std::move(run_iteration)();
  }

  void NoOp() {}

  static void Reset() {
    fuzz_test_runner_called = 0;
    fuzz_test_iteration_runner_called = 0;
  }

  static int fuzz_test_runner_called;
  static int fuzz_test_iteration_runner_called;
};

int ExampleRunnerFixture::fuzz_test_runner_called = 0;
int ExampleRunnerFixture::fuzz_test_iteration_runner_called = 0;

TEST(FixtureDriverTest, FixtureRunnerFunctionsAreCalled) {
  using NoOpFunc = decltype(&ExampleRunnerFixture::NoOp);
  FixtureDriverImpl<Domain<std::tuple<>>, ExampleRunnerFixture, NoOpFunc, void*>
      fixture_driver(&ExampleRunnerFixture::NoOp, Arbitrary<std::tuple<>>(), {},
                     nullptr);

  ExampleRunnerFixture::Reset();

  fixture_driver.RunFuzzTest([&] {
    EXPECT_EQ(ExampleRunnerFixture::fuzz_test_runner_called, 1);
    fixture_driver.RunFuzzTestIteration([&] {
      EXPECT_EQ(ExampleRunnerFixture::fuzz_test_iteration_runner_called, 1);
    });
    fixture_driver.RunFuzzTestIteration([&] {
      EXPECT_EQ(ExampleRunnerFixture::fuzz_test_iteration_runner_called, 2);
    });
  });
}

template <typename T>
std::vector<T> UnpackGenericValues(
    absl::Span<const CopyableAny> generic_values) {
  std::vector<T> values;
  values.reserve(generic_values.size());
  for (const auto& generic_value : generic_values) {
    FUZZTEST_INTERNAL_CHECK(generic_value.Has<T>(),
                            "Generic value of a wrong type.");
    values.push_back(generic_value.GetAs<T>());
  }
  return values;
}

void TakesInt(int) {}
std::vector<std::tuple<int>> GetSeeds() { return {{7}, {42}}; }

TEST(FixtureDriverTest, PropagatesSeedsFromFreeSeedProvider) {
  FixtureDriverImpl<Domain<std::tuple<int>>, NoFixture, decltype(&TakesInt),
                    decltype(GetSeeds)>
      fixture_driver(&TakesInt, Arbitrary<std::tuple<int>>(), {}, GetSeeds);

  EXPECT_THAT(UnpackGenericValues<std::tuple<int>>(fixture_driver.GetSeeds()),
              UnorderedElementsAre(std::tuple{7}, std::tuple{42}));
}

struct FixtureWithSeedProvider {
  void TakesInt(int) {}
  std::vector<std::tuple<int>> GetSeeds() { return {{7}, {42}}; }
};

TEST(FixtureDriverTest, PropagatesSeedsFromSeedProviderOnFixture) {
  auto seed_provided = &FixtureWithSeedProvider::GetSeeds;
  FixtureDriverImpl<Domain<std::tuple<int>>, FixtureWithSeedProvider,
                    decltype(&FixtureWithSeedProvider::TakesInt),
                    decltype(seed_provided)>
      fixture_driver(&FixtureWithSeedProvider::TakesInt,
                     Arbitrary<std::tuple<int>>(), {}, seed_provided);
  fixture_driver.RunFuzzTest([&] {
    EXPECT_THAT(UnpackGenericValues<std::tuple<int>>(fixture_driver.GetSeeds()),
                UnorderedElementsAre(std::tuple{7}, std::tuple{42}));
  });
}

struct DerivedFixtureWithSeedProvider : FixtureWithSeedProvider {};

TEST(FixtureDriverTest, PropagatesSeedsFromSeedProviderOnBaseFixture) {
  auto seed_provided = &DerivedFixtureWithSeedProvider::GetSeeds;
  FixtureDriverImpl<Domain<std::tuple<int>>, DerivedFixtureWithSeedProvider,
                    decltype(&DerivedFixtureWithSeedProvider::TakesInt),
                    decltype(seed_provided)>
      fixture_driver(&DerivedFixtureWithSeedProvider::TakesInt,
                     Arbitrary<std::tuple<int>>(), {}, seed_provided);
  fixture_driver.RunFuzzTest([&] {
    EXPECT_THAT(UnpackGenericValues<std::tuple<int>>(fixture_driver.GetSeeds()),
                UnorderedElementsAre(std::tuple{7}, std::tuple{42}));
  });
}

TEST(FixtureDriverTest, InvalidSeedsFromSeedProviderAreSkipped) {
  FixtureDriverImpl<Domain<std::tuple<int>>, NoFixture, decltype(&TakesInt),
                    decltype(GetSeeds)>
      fixture_driver(
          &TakesInt,
          Filter([](std::tuple<int> i) { return std::get<0>(i) % 2 == 0; },
                 Arbitrary<std::tuple<int>>()),
          {}, GetSeeds);

  fixture_driver.RunFuzzTest([&] {
    EXPECT_THAT(
        UnpackGenericValues<std::tuple<int>>(
            UnpackGenericValues<CopyableAny>(fixture_driver.GetSeeds())),
        UnorderedElementsAre(std::tuple{42}));
  });
}

}  // namespace
}  // namespace fuzztest::internal
