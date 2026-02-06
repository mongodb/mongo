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

#include "./centipede/runner_flags.h"

#include "gtest/gtest.h"

namespace fuzztest::internal {

namespace {

TEST(RunnerFlags, Empty) {
  RunnerFlags runner_flags("");
  EXPECT_TRUE(runner_flags.ToString().empty());
}

TEST(RunnerFlags, Malformed) {
  RunnerFlags runner_flags("a=x:b=:=c");
  EXPECT_EQ(runner_flags.ToString(), ":a=x:b:");
}

TEST(RunnerFlags, HasFlag) {
  RunnerFlags runner_flags(":a:b=x:");

  EXPECT_TRUE(runner_flags.HasFlag("a"));
  EXPECT_TRUE(runner_flags.HasFlag("b"));
  EXPECT_FALSE(runner_flags.HasFlag("c"));
}

TEST(RunnerFlags, GetFlagValue) {
  RunnerFlags runner_flags(":a=x:b:");

  EXPECT_EQ(runner_flags.GetFlagValue("a"), "x");
  EXPECT_TRUE(runner_flags.GetFlagValue("b").empty());
  EXPECT_TRUE(runner_flags.GetFlagValue("c").empty());
}

TEST(RunnerFlags, RepeatedFlag) {
  RunnerFlags runner_flags(":a=x:a=y:");
  EXPECT_EQ(runner_flags.GetFlagValue("a"), "y");
}

TEST(RunnerFlags, SetFlagValue) {
  RunnerFlags runner_flags(":a=x:b:");

  // Multiple updates.
  runner_flags.SetFlagValue("a", "red");
  EXPECT_EQ(runner_flags.GetFlagValue("a"), "red");
  runner_flags.SetFlagValue("a", "green");
  EXPECT_EQ(runner_flags.GetFlagValue("a"), "green");

  // Changing a flag to valueless.
  runner_flags.SetFlagValue("a", "");
  EXPECT_TRUE(runner_flags.GetFlagValue("a").empty());

  // Adding value to an existing flag without value.
  runner_flags.SetFlagValue("b", "yellow");
  EXPECT_EQ(runner_flags.GetFlagValue("b"), "yellow");

  // Adding a new flag.
  runner_flags.SetFlagValue("c", "blue");
  EXPECT_EQ(runner_flags.GetFlagValue("c"), "blue");
}

TEST(RunnerFlags, ToString) {
  RunnerFlags runner_flags(":b=x:a:c=y:");
  EXPECT_EQ(runner_flags.ToString(), ":b=x:a:c=y:");
}
}  // namespace
}  // namespace fuzztest::internal
