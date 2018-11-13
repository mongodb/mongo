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

#include "absl/strings/internal/charconv_bigint.h"

#include <string>

#include "gtest/gtest.h"

namespace absl {
namespace strings_internal {

TEST(BigUnsigned, ShiftLeft) {
  {
    // Check that 3 * 2**100 is calculated correctly
    BigUnsigned<4> num(3u);
    num.ShiftLeft(100);
    EXPECT_EQ(num, BigUnsigned<4>("3802951800684688204490109616128"));
  }
  {
    // Test that overflow is truncated properly.
    // 15 is 4 bits long, and BigUnsigned<4> is a 128-bit bigint.
    // Shifting left by 125 bits should truncate off the high bit, so that
    //   15 << 125 == 7 << 125
    // after truncation.
    BigUnsigned<4> a(15u);
    BigUnsigned<4> b(7u);
    BigUnsigned<4> c(3u);
    a.ShiftLeft(125);
    b.ShiftLeft(125);
    c.ShiftLeft(125);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
  }
  {
    // Same test, larger bigint:
    BigUnsigned<84> a(15u);
    BigUnsigned<84> b(7u);
    BigUnsigned<84> c(3u);
    a.ShiftLeft(84 * 32 - 3);
    b.ShiftLeft(84 * 32 - 3);
    c.ShiftLeft(84 * 32 - 3);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
  }
  {
    // Check that incrementally shifting has the same result as doing it all at
    // once (attempting to capture corner cases.)
    const std::string seed = "1234567890123456789012345678901234567890";
    BigUnsigned<84> a(seed);
    for (int i = 1; i <= 84 * 32; ++i) {
      a.ShiftLeft(1);
      BigUnsigned<84> b(seed);
      b.ShiftLeft(i);
      EXPECT_EQ(a, b);
    }
    // And we should have fully rotated all bits off by now:
    EXPECT_EQ(a, BigUnsigned<84>(0u));
  }
}

TEST(BigUnsigned, MultiplyByUint32) {
  const BigUnsigned<84> factorial_100(
      "933262154439441526816992388562667004907159682643816214685929638952175999"
      "932299156089414639761565182862536979208272237582511852109168640000000000"
      "00000000000000");
  BigUnsigned<84> a(1u);
  for (uint32_t i = 1; i <= 100; ++i) {
    a.MultiplyBy(i);
  }
  EXPECT_EQ(a, BigUnsigned<84>(factorial_100));
}

TEST(BigUnsigned, MultiplyByBigUnsigned) {
  {
    // Put the terms of factorial_200 into two bigints, and multiply them
    // together.
    const BigUnsigned<84> factorial_200(
        "7886578673647905035523632139321850622951359776871732632947425332443594"
        "4996340334292030428401198462390417721213891963883025764279024263710506"
        "1926624952829931113462857270763317237396988943922445621451664240254033"
        "2918641312274282948532775242424075739032403212574055795686602260319041"
        "7032406235170085879617892222278962370389737472000000000000000000000000"
        "0000000000000000000000000");
    BigUnsigned<84> evens(1u);
    BigUnsigned<84> odds(1u);
    for (uint32_t i = 1; i < 200; i += 2) {
      odds.MultiplyBy(i);
      evens.MultiplyBy(i + 1);
    }
    evens.MultiplyBy(odds);
    EXPECT_EQ(evens, factorial_200);
  }
  {
    // Multiply various powers of 10 together.
    for (int a = 0 ; a < 700; a += 25) {
      SCOPED_TRACE(a);
      BigUnsigned<84> a_value("3" + std::string(a, '0'));
      for (int b = 0; b < (700 - a); b += 25) {
        SCOPED_TRACE(b);
        BigUnsigned<84> b_value("2" + std::string(b, '0'));
        BigUnsigned<84> expected_product("6" + std::string(a + b, '0'));
        b_value.MultiplyBy(a_value);
        EXPECT_EQ(b_value, expected_product);
      }
    }
  }
}

TEST(BigUnsigned, MultiplyByOverflow) {
  {
    // Check that multiplcation overflow predictably truncates.

    // A big int with all bits on.
    BigUnsigned<4> all_bits_on("340282366920938463463374607431768211455");
    // Modulo 2**128, this is equal to -1.  Therefore the square of this,
    // modulo 2**128, should be 1.
    all_bits_on.MultiplyBy(all_bits_on);
    EXPECT_EQ(all_bits_on, BigUnsigned<4>(1u));
  }
  {
    // Try multiplying a large bigint by 2**50, and compare the result to
    // shifting.
    BigUnsigned<4> value_1("12345678901234567890123456789012345678");
    BigUnsigned<4> value_2("12345678901234567890123456789012345678");
    BigUnsigned<4> two_to_fiftieth(1u);
    two_to_fiftieth.ShiftLeft(50);

    value_1.ShiftLeft(50);
    value_2.MultiplyBy(two_to_fiftieth);
    EXPECT_EQ(value_1, value_2);
  }
}

TEST(BigUnsigned, FiveToTheNth) {
  {
    // Sanity check that MultiplyByFiveToTheNth gives consistent answers, up to
    // and including overflow.
    for (int i = 0; i < 1160; ++i) {
      SCOPED_TRACE(i);
      BigUnsigned<84> value_1(123u);
      BigUnsigned<84> value_2(123u);
      value_1.MultiplyByFiveToTheNth(i);
      for (int j = 0; j < i; j++) {
        value_2.MultiplyBy(5u);
      }
      EXPECT_EQ(value_1, value_2);
    }
  }
  {
    // Check that the faster, table-lookup-based static method returns the same
    // result that multiplying in-place would return, up to and including
    // overflow.
    for (int i = 0; i < 1160; ++i) {
      SCOPED_TRACE(i);
      BigUnsigned<84> value_1(1u);
      value_1.MultiplyByFiveToTheNth(i);
      BigUnsigned<84> value_2 = BigUnsigned<84>::FiveToTheNth(i);
      EXPECT_EQ(value_1, value_2);
    }
  }
}

TEST(BigUnsigned, TenToTheNth) {
  {
    // Sanity check MultiplyByTenToTheNth.
    for (int i = 0; i < 800; ++i) {
      SCOPED_TRACE(i);
      BigUnsigned<84> value_1(123u);
      BigUnsigned<84> value_2(123u);
      value_1.MultiplyByTenToTheNth(i);
      for (int j = 0; j < i; j++) {
        value_2.MultiplyBy(10u);
      }
      EXPECT_EQ(value_1, value_2);
    }
  }
  {
    // Alternate testing approach, taking advantage of the decimal parser.
    for (int i = 0; i < 200; ++i) {
      SCOPED_TRACE(i);
      BigUnsigned<84> value_1(135u);
      value_1.MultiplyByTenToTheNth(i);
      BigUnsigned<84> value_2("135" + std::string(i, '0'));
      EXPECT_EQ(value_1, value_2);
    }
  }
}


}  // namespace strings_internal
}  // namespace absl
