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

#include "./centipede/config_util.h"

#include <string>
#include <string_view>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/flags/flag.h"
#include "./centipede/environment_flags.h"
#include "./common/logging.h"

// Dummy flags for testing.
ABSL_FLAG(std::string, foo, "bar", "foo help");
ABSL_FLAG(bool, qux, false, "qux help");

namespace fuzztest::internal {

// NOTE: Has to be outside the anonymous namespace.
bool operator==(const FlagInfo& tested, const FlagInfo& expected) {
  return tested.name == expected.name &&
         (expected.value == "*" || tested.value == expected.value) &&
         (expected.default_value == "*" ||
          tested.default_value == expected.default_value) &&
         (expected.help == "*" || tested.help == expected.help);
}

namespace {

using ::testing::ElementsAreArray;
using ::testing::IsSupersetOf;

TEST(FlagUtilTest, GetFlagsPerSource) {
  constexpr const char* kCentipedeRoot = "centipede/";
  constexpr const char* kThisCc = "centipede/config_util_test.cc";
  constexpr const char* kCentipedeFlagsInc =
      "././centipede/centipede_flags.inc";

  // Change some flag values to non-defaults.
  absl::SetFlag(&FLAGS_foo, "baz");
  absl::SetFlag(&FLAGS_qux, true);
  // Create a dummy Environment to touch its flags and prevent them from being
  // optimized out.
  [[maybe_unused]] auto dummy_env = CreateEnvironmentFromFlags();

  // All centipede/ modules.
  {
    const FlagInfosPerSource flags = GetFlagsPerSource(kCentipedeRoot);
    SCOPED_TRACE(FormatFlagfileString(flags));
    ASSERT_EQ(flags.count(kThisCc), 1);
    ASSERT_EQ(flags.count(kCentipedeFlagsInc), 1);
    ASSERT_THAT(flags.at(kThisCc),
                ElementsAreArray({
                    FlagInfo{"foo", "baz", "bar", "foo help"},
                    FlagInfo{"qux", "true", "false", "qux help"},
                }));
    ASSERT_THAT(flags.at(kCentipedeFlagsInc),
                IsSupersetOf({
                    FlagInfo{"binary", "*", "*", "*"},
                    FlagInfo{"workdir", "*", "*", "*"},
                }));
  }
  // Just this file.
  {
    const FlagInfosPerSource flags = GetFlagsPerSource(kThisCc);
    SCOPED_TRACE(FormatFlagfileString(flags));
    ASSERT_EQ(flags.count(kThisCc), 1);
    ASSERT_EQ(flags.count(kCentipedeFlagsInc), 0);
    ASSERT_THAT(flags.at(kThisCc),
                ElementsAreArray({
                    FlagInfo{"foo", "baz", "bar", "foo help"},
                    FlagInfo{"qux", "true", "false", "qux help"},
                }));
  }
  // Just this file with one flag excluded.
  {
    const FlagInfosPerSource flags =
        GetFlagsPerSource(kThisCc, /*exclude_flags=*/{"qux"});
    SCOPED_TRACE(FormatFlagfileString(flags));
    ASSERT_EQ(flags.count(kThisCc), 1);
    ASSERT_EQ(flags.count(kCentipedeFlagsInc), 0);
    ASSERT_THAT(flags.at(kThisCc),
                ElementsAreArray({
                    FlagInfo{"foo", "baz", "bar", "foo help"},
                }));
  }
}

TEST(FlagUtilTest, FormatFlagfileString) {
  // NOTE: Everything is intentionally unsorted: the result is expected to be
  // sorted by file, then by flag name.
  const FlagInfosPerSource kFlags = {
      {"bob.cc",
       {
           FlagInfo{"bob_x", "bob_x def", "bob_x def", "bob_x help"},
           FlagInfo{"bob_y", "bob_y val", "bob_y def", "bob_y help"},
       }},
      {"alice.cc",
       {
           FlagInfo{"alice_x", "alice_x val", "alice_x def", "alice_x help"},
           FlagInfo{"alice_y", "alice_y val", "alice_y def", "alice_y help"},
           FlagInfo{"alice_z", "alice_z def", "alice_z def", "alice_z help"},
       }},
  };

  struct TestCase {
    DefaultedFlags defaulted;
    FlagComments comments;
    std::string_view expected_flagfile_string;
  };

  TestCase kTestCases[] = {
      {DefaultedFlags::kExcluded, FlagComments::kNone,
       R"(# NOTE: Defaulted flags are excluded

# Flags from alice.cc:
  --alice_x=alice_x val
  --alice_y=alice_y val

# Flags from bob.cc:
  --bob_y=bob_y val
)"},
      {DefaultedFlags::kIncluded, FlagComments::kNone,
       R"(# NOTE: Explicit and defaulted flags are included

# Flags from alice.cc:
  --alice_x=alice_x val
  --alice_y=alice_y val
  --alice_z=alice_z def

# Flags from bob.cc:
  --bob_x=bob_x def
  --bob_y=bob_y val
)"},
      {DefaultedFlags::kCommentedOut, FlagComments::kNone,
       R"(# NOTE: Defaulted flags are commented out

# Flags from alice.cc:
  --alice_x=alice_x val
  --alice_y=alice_y val
  # --alice_z=alice_z def

# Flags from bob.cc:
  # --bob_x=bob_x def
  --bob_y=bob_y val
)"},
      {DefaultedFlags::kIncluded, FlagComments::kDefault,
       R"(# NOTE: Explicit and defaulted flags are included

# Flags from alice.cc:
  # default: 'alice_x def'
  --alice_x=alice_x val

  # default: 'alice_y def'
  --alice_y=alice_y val

  # default: 'alice_z def'
  --alice_z=alice_z def

# Flags from bob.cc:
  # default: 'bob_x def'
  --bob_x=bob_x def

  # default: 'bob_y def'
  --bob_y=bob_y val
)"},
      {DefaultedFlags::kIncluded, FlagComments::kHelpAndDefault,
       R"(# NOTE: Explicit and defaulted flags are included

# Flags from alice.cc:
  # alice_x help
  # default: 'alice_x def'
  --alice_x=alice_x val

  # alice_y help
  # default: 'alice_y def'
  --alice_y=alice_y val

  # alice_z help
  # default: 'alice_z def'
  --alice_z=alice_z def

# Flags from bob.cc:
  # bob_x help
  # default: 'bob_x def'
  --bob_x=bob_x def

  # bob_y help
  # default: 'bob_y def'
  --bob_y=bob_y val
)"},
      {DefaultedFlags::kCommentedOut, FlagComments::kHelpAndDefault,
       R"(# NOTE: Defaulted flags are commented out

# Flags from alice.cc:
  # alice_x help
  # default: 'alice_x def'
  --alice_x=alice_x val

  # alice_y help
  # default: 'alice_y def'
  --alice_y=alice_y val

  # alice_z help
  # default: 'alice_z def'
  # --alice_z=alice_z def

# Flags from bob.cc:
  # bob_x help
  # default: 'bob_x def'
  # --bob_x=bob_x def

  # bob_y help
  # default: 'bob_y def'
  --bob_y=bob_y val
)"},
  };

  for (const auto& test_case : kTestCases) {
    const std::string flagfile_string =
        FormatFlagfileString(kFlags, test_case.defaulted, test_case.comments);
    EXPECT_EQ(flagfile_string, test_case.expected_flagfile_string)
        << "\n--------\n"
        << VV(flagfile_string) << "--------\n"
        << VV(test_case.expected_flagfile_string) << "--------\n"
        << VV(static_cast<int>(test_case.defaulted))
        << VV(static_cast<int>(test_case.comments));
  }
}

}  // namespace
}  // namespace fuzztest::internal
