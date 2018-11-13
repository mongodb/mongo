// Copyright 2018 The Abseil Authors.
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

#include "absl/memory/memory.h"

#include "gtest/gtest.h"
#include "absl/base/internal/exception_safety_testing.h"

namespace absl {
namespace {

constexpr int kLength = 50;
using Thrower = testing::ThrowingValue<testing::TypeSpec::kEverythingThrows>;
using ThrowerStorage =
    absl::aligned_storage_t<sizeof(Thrower), alignof(Thrower)>;
using ThrowerList = std::array<ThrowerStorage, kLength>;

TEST(MakeUnique, CheckForLeaks) {
  constexpr int kValue = 321;
  auto tester = testing::MakeExceptionSafetyTester()
                    .WithInitialValue(Thrower(kValue))
                    // Ensures make_unique does not modify the input. The real
                    // test, though, is ConstructorTracker checking for leaks.
                    .WithContracts(testing::strong_guarantee);

  EXPECT_TRUE(tester.Test([](Thrower* thrower) {
    static_cast<void>(absl::make_unique<Thrower>(*thrower));
  }));

  EXPECT_TRUE(tester.Test([](Thrower* thrower) {
    static_cast<void>(absl::make_unique<Thrower>(std::move(*thrower)));
  }));

  // Test T[n] overload
  EXPECT_TRUE(tester.Test([&](Thrower*) {
    static_cast<void>(absl::make_unique<Thrower[]>(kLength));
  }));
}

}  // namespace
}  // namespace absl
