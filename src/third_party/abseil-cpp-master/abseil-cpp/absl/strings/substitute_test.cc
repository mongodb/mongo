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

#include "absl/strings/substitute.h"

#include <cstdint>

#include "gtest/gtest.h"
#include "absl/strings/str_cat.h"

namespace {

TEST(SubstituteTest, Substitute) {
  // Basic.
  EXPECT_EQ("Hello, world!", absl::Substitute("$0, $1!", "Hello", "world"));

  // Non-char* types.
  EXPECT_EQ("123 0.2 0.1 foo true false x",
            absl::Substitute("$0 $1 $2 $3 $4 $5 $6", 123, 0.2, 0.1f,
                             std::string("foo"), true, false, 'x'));

  // All int types.
  EXPECT_EQ(
      "-32767 65535 "
      "-1234567890 3234567890 "
      "-1234567890 3234567890 "
      "-1234567890123456789 9234567890123456789",
      absl::Substitute(
          "$0 $1 $2 $3 $4 $5 $6 $7",
          static_cast<short>(-32767),          // NOLINT(runtime/int)
          static_cast<unsigned short>(65535),  // NOLINT(runtime/int)
          -1234567890, 3234567890U, -1234567890L, 3234567890UL,
          -int64_t{1234567890123456789}, uint64_t{9234567890123456789u}));

  // Hex format
  EXPECT_EQ("0 1 f ffff0ffff 0123456789abcdef",
            absl::Substitute("$0$1$2$3$4 $5",  //
                             absl::Hex(0), absl::Hex(1, absl::kSpacePad2),
                             absl::Hex(0xf, absl::kSpacePad2),
                             absl::Hex(int16_t{-1}, absl::kSpacePad5),
                             absl::Hex(int16_t{-1}, absl::kZeroPad5),
                             absl::Hex(0x123456789abcdef, absl::kZeroPad16)));

  // Dec format
  EXPECT_EQ("0 115   -1-0001 81985529216486895",
            absl::Substitute("$0$1$2$3$4 $5",  //
                             absl::Dec(0), absl::Dec(1, absl::kSpacePad2),
                             absl::Dec(0xf, absl::kSpacePad2),
                             absl::Dec(int16_t{-1}, absl::kSpacePad5),
                             absl::Dec(int16_t{-1}, absl::kZeroPad5),
                             absl::Dec(0x123456789abcdef, absl::kZeroPad16)));

  // Pointer.
  const int* int_p = reinterpret_cast<const int*>(0x12345);
  std::string str = absl::Substitute("$0", int_p);
  EXPECT_EQ(absl::StrCat("0x", absl::Hex(int_p)), str);

  // Volatile Pointer.
  // Like C++ streamed I/O, such pointers implicitly become bool
  volatile int vol = 237;
  volatile int *volatile volptr = &vol;
  str = absl::Substitute("$0", volptr);
  EXPECT_EQ("true", str);

  // null is special. StrCat prints 0x0. Substitute prints NULL.
  const uint64_t* null_p = nullptr;
  str = absl::Substitute("$0", null_p);
  EXPECT_EQ("NULL", str);

  // char* is also special.
  const char* char_p = "print me";
  str = absl::Substitute("$0", char_p);
  EXPECT_EQ("print me", str);

  char char_buf[16];
  strncpy(char_buf, "print me too", sizeof(char_buf));
  str = absl::Substitute("$0", char_buf);
  EXPECT_EQ("print me too", str);

  // null char* is "doubly" special. Represented as the empty std::string.
  char_p = nullptr;
  str = absl::Substitute("$0", char_p);
  EXPECT_EQ("", str);

  // Out-of-order.
  EXPECT_EQ("b, a, c, b", absl::Substitute("$1, $0, $2, $1", "a", "b", "c"));

  // Literal $
  EXPECT_EQ("$", absl::Substitute("$$"));

  EXPECT_EQ("$1", absl::Substitute("$$1"));

  // Test all overloads.
  EXPECT_EQ("a", absl::Substitute("$0", "a"));
  EXPECT_EQ("a b", absl::Substitute("$0 $1", "a", "b"));
  EXPECT_EQ("a b c", absl::Substitute("$0 $1 $2", "a", "b", "c"));
  EXPECT_EQ("a b c d", absl::Substitute("$0 $1 $2 $3", "a", "b", "c", "d"));
  EXPECT_EQ("a b c d e",
            absl::Substitute("$0 $1 $2 $3 $4", "a", "b", "c", "d", "e"));
  EXPECT_EQ("a b c d e f", absl::Substitute("$0 $1 $2 $3 $4 $5", "a", "b", "c",
                                            "d", "e", "f"));
  EXPECT_EQ("a b c d e f g", absl::Substitute("$0 $1 $2 $3 $4 $5 $6", "a", "b",
                                              "c", "d", "e", "f", "g"));
  EXPECT_EQ("a b c d e f g h",
            absl::Substitute("$0 $1 $2 $3 $4 $5 $6 $7", "a", "b", "c", "d", "e",
                             "f", "g", "h"));
  EXPECT_EQ("a b c d e f g h i",
            absl::Substitute("$0 $1 $2 $3 $4 $5 $6 $7 $8", "a", "b", "c", "d",
                             "e", "f", "g", "h", "i"));
  EXPECT_EQ("a b c d e f g h i j",
            absl::Substitute("$0 $1 $2 $3 $4 $5 $6 $7 $8 $9", "a", "b", "c",
                             "d", "e", "f", "g", "h", "i", "j"));
  EXPECT_EQ("a b c d e f g h i j b0",
            absl::Substitute("$0 $1 $2 $3 $4 $5 $6 $7 $8 $9 $10", "a", "b", "c",
                             "d", "e", "f", "g", "h", "i", "j"));

  const char* null_cstring = nullptr;
  EXPECT_EQ("Text: ''", absl::Substitute("Text: '$0'", null_cstring));
}

TEST(SubstituteTest, SubstituteAndAppend) {
  std::string str = "Hello";
  absl::SubstituteAndAppend(&str, ", $0!", "world");
  EXPECT_EQ("Hello, world!", str);

  // Test all overloads.
  str.clear();
  absl::SubstituteAndAppend(&str, "$0", "a");
  EXPECT_EQ("a", str);
  str.clear();
  absl::SubstituteAndAppend(&str, "$0 $1", "a", "b");
  EXPECT_EQ("a b", str);
  str.clear();
  absl::SubstituteAndAppend(&str, "$0 $1 $2", "a", "b", "c");
  EXPECT_EQ("a b c", str);
  str.clear();
  absl::SubstituteAndAppend(&str, "$0 $1 $2 $3", "a", "b", "c", "d");
  EXPECT_EQ("a b c d", str);
  str.clear();
  absl::SubstituteAndAppend(&str, "$0 $1 $2 $3 $4", "a", "b", "c", "d", "e");
  EXPECT_EQ("a b c d e", str);
  str.clear();
  absl::SubstituteAndAppend(&str, "$0 $1 $2 $3 $4 $5", "a", "b", "c", "d", "e",
                            "f");
  EXPECT_EQ("a b c d e f", str);
  str.clear();
  absl::SubstituteAndAppend(&str, "$0 $1 $2 $3 $4 $5 $6", "a", "b", "c", "d",
                            "e", "f", "g");
  EXPECT_EQ("a b c d e f g", str);
  str.clear();
  absl::SubstituteAndAppend(&str, "$0 $1 $2 $3 $4 $5 $6 $7", "a", "b", "c", "d",
                            "e", "f", "g", "h");
  EXPECT_EQ("a b c d e f g h", str);
  str.clear();
  absl::SubstituteAndAppend(&str, "$0 $1 $2 $3 $4 $5 $6 $7 $8", "a", "b", "c",
                            "d", "e", "f", "g", "h", "i");
  EXPECT_EQ("a b c d e f g h i", str);
  str.clear();
  absl::SubstituteAndAppend(&str, "$0 $1 $2 $3 $4 $5 $6 $7 $8 $9", "a", "b",
                            "c", "d", "e", "f", "g", "h", "i", "j");
  EXPECT_EQ("a b c d e f g h i j", str);
}

#ifdef GTEST_HAS_DEATH_TEST

TEST(SubstituteDeathTest, SubstituteDeath) {
  EXPECT_DEBUG_DEATH(
      static_cast<void>(absl::Substitute(absl::string_view("-$2"), "a", "b")),
      "Invalid strings::Substitute\\(\\) format std::string: asked for \"\\$2\", "
      "but only 2 args were given.");
  EXPECT_DEBUG_DEATH(
      static_cast<void>(absl::Substitute("-$z-")),
      "Invalid strings::Substitute\\(\\) format std::string: \"-\\$z-\"");
  EXPECT_DEBUG_DEATH(
      static_cast<void>(absl::Substitute("-$")),
      "Invalid strings::Substitute\\(\\) format std::string: \"-\\$\"");
}

#endif  // GTEST_HAS_DEATH_TEST

}  // namespace
