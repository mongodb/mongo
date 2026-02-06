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

#include "./centipede/config_file.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace fuzztest::internal {
namespace {

TEST(ConfigFileTest, AugmentedArgv) {
  const std::vector<std::string> kOrigArgv = {
      "--foo=bar", "-baz", "-bazz", "baz", "qux",
  };

  // None of the replacements match.
  {
    const AugmentedArgvWithCleanup augmented_argv{
        kOrigArgv,
        /*flag_replacements=*/{},
        /*replacements=*/
        {
            {"mismatching", "mod_mismatching"},
        },
        nullptr};
    EXPECT_FALSE(augmented_argv.was_augmented());
    EXPECT_EQ(augmented_argv.argv()[0], kOrigArgv[0]);
    EXPECT_EQ(augmented_argv.argv()[1], kOrigArgv[1]);
    EXPECT_EQ(augmented_argv.argv()[2], kOrigArgv[2]);
    EXPECT_EQ(augmented_argv.argv()[3], kOrigArgv[3]);
    EXPECT_EQ(augmented_argv.argv()[4], kOrigArgv[4]);
  }

  // The replacements match and the cleanup runs as a result.
  {
    bool cleanup_worked = false;
    {
      const AugmentedArgvWithCleanup augmented_argv{
          kOrigArgv,
          /*flag_replacements=*/
          {
              {"foo", "mod_foo"},
              {"baz", "mod_baz"},
          },
          /*replacements=*/
          {
              {"bar", "mod_bar"},
              {"qux", "mod_qux"},
          },
          [&cleanup_worked]() { cleanup_worked = true; }};
      const std::vector<std::string> kExpectedArgv = {
          "--mod_foo=mod_bar",
          "-mod_baz",
          // Flag replacement should skip this item because the flag name
          // does not match as a whole.
          "-bazz",
          // Flag replacement should skip this item because it's not a flag.
          "baz",
          "mod_qux",
      };
      EXPECT_TRUE(augmented_argv.was_augmented());
      EXPECT_EQ(augmented_argv.argv()[0], kExpectedArgv[0]);
      EXPECT_EQ(augmented_argv.argv()[1], kExpectedArgv[1]);
      EXPECT_EQ(augmented_argv.argv()[2], kExpectedArgv[2]);
      EXPECT_EQ(augmented_argv.argv()[3], kExpectedArgv[3]);
      EXPECT_EQ(augmented_argv.argv()[4], kExpectedArgv[4]);
    }
    EXPECT_TRUE(cleanup_worked);
  }
}

// TODO(ussuri): The rest of the module is tested by calling Centipede with
//  the new flags in centipede_main_cns_test.sh. Consider adding proper C++
//  tests here too.

}  // namespace
}  // namespace fuzztest::internal
