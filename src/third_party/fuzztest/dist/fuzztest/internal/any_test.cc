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

#include "./fuzztest/internal/any.h"

#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "gtest/gtest.h"
#include "absl/strings/str_cat.h"

namespace fuzztest::internal {
namespace {

TEST(CopyableAny, BasicOperationsWork) {
  CopyableAny v(std::in_place_type<int>, 17);
  ASSERT_TRUE(v.Has<int>());
  EXPECT_FALSE(v.Has<double>());
  EXPECT_EQ(v.GetAs<int>(), 17);

  {
    auto copy = v;
    ASSERT_TRUE(copy.Has<int>());
    EXPECT_FALSE(copy.Has<double>());
    EXPECT_EQ(v.GetAs<int>(), 17);
    EXPECT_EQ(copy.GetAs<int>(), 17);
  }

  // Original should still have the value even after the copy is gone.
  EXPECT_EQ(v.GetAs<int>(), 17);
}

TEST(CopyableAny, NonTrivialTypesAreCopiedAndDestroyedCorrectly) {
  std::string str = "A very long string that uses heap.";
  CopyableAny v(std::in_place_type<std::string>, str);
  ASSERT_TRUE(v.Has<std::string>());
  EXPECT_EQ(str, v.GetAs<std::string>());
  auto copy = v;
  ASSERT_TRUE(v.Has<std::string>());
  ASSERT_TRUE(copy.Has<std::string>());
  EXPECT_EQ(str, v.GetAs<std::string>());
  EXPECT_EQ(str, copy.GetAs<std::string>());
  auto moved = std::move(copy);
  ASSERT_TRUE(v.Has<std::string>());
  EXPECT_FALSE(copy.Has<std::string>());
  ASSERT_TRUE(moved.Has<std::string>());
  EXPECT_EQ(str, v.GetAs<std::string>());
  EXPECT_EQ(str, moved.GetAs<std::string>());
}

TEST(MoveOnlyAny, BasicOperationsWork) {
  MoveOnlyAny v(std::in_place_type<int>, 17);
  ASSERT_TRUE(v.Has<int>());
  EXPECT_FALSE(v.Has<double>());
  EXPECT_EQ(v.GetAs<int>(), 17);

  auto other = std::move(v);
  ASSERT_TRUE(other.Has<int>());
  EXPECT_FALSE(other.Has<double>());
  EXPECT_EQ(other.GetAs<int>(), 17);

  EXPECT_FALSE(v.has_value());
}

TEST(MoveOnlyAny, NonTrivialTypesAreCopiedAndDestroyedCorrectly) {
  auto str =
      std::make_unique<std::string>("A very long string that uses heap.");
  MoveOnlyAny v(std::in_place_type<std::unique_ptr<std::string>>,
                std::make_unique<std::string>(*str));
  ASSERT_TRUE(v.Has<std::unique_ptr<std::string>>());
  EXPECT_EQ(*str, *v.GetAs<std::unique_ptr<std::string>>());
  auto* p = &v.GetAs<std::unique_ptr<std::string>>();
  auto moved = std::move(v);
  EXPECT_FALSE(v.Has<std::unique_ptr<std::string>>());
  ASSERT_TRUE(moved.Has<std::unique_ptr<std::string>>());
  EXPECT_EQ(*str, *moved.GetAs<std::unique_ptr<std::string>>());
  EXPECT_EQ(p, &moved.GetAs<std::unique_ptr<std::string>>());
}

}  // namespace
}  // namespace fuzztest::internal
