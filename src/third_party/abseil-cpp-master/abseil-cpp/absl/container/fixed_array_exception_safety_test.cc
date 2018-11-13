// Copyright 2017 The Abseil Authors.
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

#include <initializer_list>

#include "absl/container/fixed_array.h"

#include "gtest/gtest.h"
#include "absl/base/internal/exception_safety_testing.h"

namespace absl {

namespace {

constexpr size_t kInlined = 25;
constexpr size_t kSmallSize = kInlined / 2;
constexpr size_t kLargeSize = kInlined * 2;

constexpr int kInitialValue = 5;
constexpr int kUpdatedValue = 10;

using ::testing::TestThrowingCtor;

using Thrower = testing::ThrowingValue<testing::TypeSpec::kEverythingThrows>;
using FixedArr = absl::FixedArray<Thrower, kInlined>;

using MoveThrower = testing::ThrowingValue<testing::TypeSpec::kNoThrowMove>;
using MoveFixedArr = absl::FixedArray<MoveThrower, kInlined>;

TEST(FixedArrayExceptionSafety, CopyConstructor) {
  auto small = FixedArr(kSmallSize);
  TestThrowingCtor<FixedArr>(small);

  auto large = FixedArr(kLargeSize);
  TestThrowingCtor<FixedArr>(large);
}

TEST(FixedArrayExceptionSafety, MoveConstructor) {
  TestThrowingCtor<FixedArr>(FixedArr(kSmallSize));
  TestThrowingCtor<FixedArr>(FixedArr(kLargeSize));

  // TypeSpec::kNoThrowMove
  TestThrowingCtor<MoveFixedArr>(MoveFixedArr(kSmallSize));
  TestThrowingCtor<MoveFixedArr>(MoveFixedArr(kLargeSize));
}

TEST(FixedArrayExceptionSafety, SizeConstructor) {
  TestThrowingCtor<FixedArr>(kSmallSize);
  TestThrowingCtor<FixedArr>(kLargeSize);
}

TEST(FixedArrayExceptionSafety, SizeValueConstructor) {
  TestThrowingCtor<FixedArr>(kSmallSize, Thrower());
  TestThrowingCtor<FixedArr>(kLargeSize, Thrower());
}

TEST(FixedArrayExceptionSafety, IteratorConstructor) {
  auto small = FixedArr(kSmallSize);
  TestThrowingCtor<FixedArr>(small.begin(), small.end());

  auto large = FixedArr(kLargeSize);
  TestThrowingCtor<FixedArr>(large.begin(), large.end());
}

TEST(FixedArrayExceptionSafety, InitListConstructor) {
  constexpr int small_inlined = 3;
  using SmallFixedArr = absl::FixedArray<Thrower, small_inlined>;

  TestThrowingCtor<SmallFixedArr>(std::initializer_list<Thrower>{});
  // Test inlined allocation
  TestThrowingCtor<SmallFixedArr>(
      std::initializer_list<Thrower>{Thrower{}, Thrower{}});
  // Test out of line allocation
  TestThrowingCtor<SmallFixedArr>(std::initializer_list<Thrower>{
      Thrower{}, Thrower{}, Thrower{}, Thrower{}, Thrower{}});
}

testing::AssertionResult ReadMemory(FixedArr* fixed_arr) {
  // Marked volatile to prevent optimization. Used for running asan tests.
  volatile int sum = 0;
  for (const auto& thrower : *fixed_arr) {
    sum += thrower.Get();
  }
  return testing::AssertionSuccess() << "Values sum to [" << sum << "]";
}

TEST(FixedArrayExceptionSafety, Fill) {
  auto test_fill = testing::MakeExceptionSafetyTester()
                       .WithContracts(ReadMemory)
                       .WithOperation([&](FixedArr* fixed_arr_ptr) {
                         auto thrower =
                             Thrower(kUpdatedValue, testing::nothrow_ctor);
                         fixed_arr_ptr->fill(thrower);
                       });

  EXPECT_TRUE(
      test_fill.WithInitialValue(FixedArr(kSmallSize, Thrower(kInitialValue)))
          .Test());
  EXPECT_TRUE(
      test_fill.WithInitialValue(FixedArr(kLargeSize, Thrower(kInitialValue)))
          .Test());
}

}  // namespace

}  // namespace absl
