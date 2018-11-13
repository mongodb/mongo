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

#include "absl/numeric/int128.h"

#include <algorithm>
#include <limits>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "absl/base/internal/cycleclock.h"
#include "absl/hash/hash_testing.h"
#include "absl/meta/type_traits.h"

#if defined(_MSC_VER) && _MSC_VER == 1900
// Disable "unary minus operator applied to unsigned type" warnings in Microsoft
// Visual C++ 14 (2015).
#pragma warning(disable:4146)
#endif

namespace {

template <typename T>
class Uint128IntegerTraitsTest : public ::testing::Test {};
typedef ::testing::Types<bool, char, signed char, unsigned char, char16_t,
                         char32_t, wchar_t,
                         short,           // NOLINT(runtime/int)
                         unsigned short,  // NOLINT(runtime/int)
                         int, unsigned int,
                         long,                // NOLINT(runtime/int)
                         unsigned long,       // NOLINT(runtime/int)
                         long long,           // NOLINT(runtime/int)
                         unsigned long long>  // NOLINT(runtime/int)
    IntegerTypes;

template <typename T>
class Uint128FloatTraitsTest : public ::testing::Test {};
typedef ::testing::Types<float, double, long double> FloatingPointTypes;

TYPED_TEST_CASE(Uint128IntegerTraitsTest, IntegerTypes);

TYPED_TEST(Uint128IntegerTraitsTest, ConstructAssignTest) {
  static_assert(std::is_constructible<absl::uint128, TypeParam>::value,
                "absl::uint128 must be constructible from TypeParam");
  static_assert(std::is_assignable<absl::uint128&, TypeParam>::value,
                "absl::uint128 must be assignable from TypeParam");
  static_assert(!std::is_assignable<TypeParam&, absl::uint128>::value,
                "TypeParam must not be assignable from absl::uint128");
}

TYPED_TEST_CASE(Uint128FloatTraitsTest, FloatingPointTypes);

TYPED_TEST(Uint128FloatTraitsTest, ConstructAssignTest) {
  static_assert(std::is_constructible<absl::uint128, TypeParam>::value,
                "absl::uint128 must be constructible from TypeParam");
  static_assert(!std::is_assignable<absl::uint128&, TypeParam>::value,
                "absl::uint128 must not be assignable from TypeParam");
  static_assert(!std::is_assignable<TypeParam&, absl::uint128>::value,
                "TypeParam must not be assignable from absl::uint128");
}

#ifdef ABSL_HAVE_INTRINSIC_INT128
// These type traits done separately as TYPED_TEST requires typeinfo, and not
// all platforms have this for __int128 even though they define the type.
TEST(Uint128, IntrinsicTypeTraitsTest) {
  static_assert(std::is_constructible<absl::uint128, __int128>::value,
                "absl::uint128 must be constructible from __int128");
  static_assert(std::is_assignable<absl::uint128&, __int128>::value,
                "absl::uint128 must be assignable from __int128");
  static_assert(!std::is_assignable<__int128&, absl::uint128>::value,
                "__int128 must not be assignable from absl::uint128");

  static_assert(std::is_constructible<absl::uint128, unsigned __int128>::value,
                "absl::uint128 must be constructible from unsigned __int128");
  static_assert(std::is_assignable<absl::uint128&, unsigned __int128>::value,
                "absl::uint128 must be assignable from unsigned __int128");
  static_assert(!std::is_assignable<unsigned __int128&, absl::uint128>::value,
                "unsigned __int128 must not be assignable from absl::uint128");
}
#endif  // ABSL_HAVE_INTRINSIC_INT128

TEST(Uint128, TrivialTraitsTest) {
  static_assert(absl::is_trivially_default_constructible<absl::uint128>::value,
                "");
  static_assert(absl::is_trivially_copy_constructible<absl::uint128>::value,
                "");
  static_assert(absl::is_trivially_copy_assignable<absl::uint128>::value, "");
  static_assert(std::is_trivially_destructible<absl::uint128>::value, "");
}

TEST(Uint128, AllTests) {
  absl::uint128 zero = 0;
  absl::uint128 one = 1;
  absl::uint128 one_2arg = absl::MakeUint128(0, 1);
  absl::uint128 two = 2;
  absl::uint128 three = 3;
  absl::uint128 big = absl::MakeUint128(2000, 2);
  absl::uint128 big_minus_one = absl::MakeUint128(2000, 1);
  absl::uint128 bigger = absl::MakeUint128(2001, 1);
  absl::uint128 biggest = absl::Uint128Max();
  absl::uint128 high_low = absl::MakeUint128(1, 0);
  absl::uint128 low_high =
      absl::MakeUint128(0, std::numeric_limits<uint64_t>::max());
  EXPECT_LT(one, two);
  EXPECT_GT(two, one);
  EXPECT_LT(one, big);
  EXPECT_LT(one, big);
  EXPECT_EQ(one, one_2arg);
  EXPECT_NE(one, two);
  EXPECT_GT(big, one);
  EXPECT_GE(big, two);
  EXPECT_GE(big, big_minus_one);
  EXPECT_GT(big, big_minus_one);
  EXPECT_LT(big_minus_one, big);
  EXPECT_LE(big_minus_one, big);
  EXPECT_NE(big_minus_one, big);
  EXPECT_LT(big, biggest);
  EXPECT_LE(big, biggest);
  EXPECT_GT(biggest, big);
  EXPECT_GE(biggest, big);
  EXPECT_EQ(big, ~~big);
  EXPECT_EQ(one, one | one);
  EXPECT_EQ(big, big | big);
  EXPECT_EQ(one, one | zero);
  EXPECT_EQ(one, one & one);
  EXPECT_EQ(big, big & big);
  EXPECT_EQ(zero, one & zero);
  EXPECT_EQ(zero, big & ~big);
  EXPECT_EQ(zero, one ^ one);
  EXPECT_EQ(zero, big ^ big);
  EXPECT_EQ(one, one ^ zero);

  // Shift operators.
  EXPECT_EQ(big, big << 0);
  EXPECT_EQ(big, big >> 0);
  EXPECT_GT(big << 1, big);
  EXPECT_LT(big >> 1, big);
  EXPECT_EQ(big, (big << 10) >> 10);
  EXPECT_EQ(big, (big >> 1) << 1);
  EXPECT_EQ(one, (one << 80) >> 80);
  EXPECT_EQ(zero, (one >> 80) << 80);

  // Shift assignments.
  absl::uint128 big_copy = big;
  EXPECT_EQ(big << 0, big_copy <<= 0);
  big_copy = big;
  EXPECT_EQ(big >> 0, big_copy >>= 0);
  big_copy = big;
  EXPECT_EQ(big << 1, big_copy <<= 1);
  big_copy = big;
  EXPECT_EQ(big >> 1, big_copy >>= 1);
  big_copy = big;
  EXPECT_EQ(big << 10, big_copy <<= 10);
  big_copy = big;
  EXPECT_EQ(big >> 10, big_copy >>= 10);
  big_copy = big;
  EXPECT_EQ(big << 64, big_copy <<= 64);
  big_copy = big;
  EXPECT_EQ(big >> 64, big_copy >>= 64);
  big_copy = big;
  EXPECT_EQ(big << 73, big_copy <<= 73);
  big_copy = big;
  EXPECT_EQ(big >> 73, big_copy >>= 73);

  EXPECT_EQ(absl::Uint128High64(biggest), std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(absl::Uint128Low64(biggest), std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(zero + one, one);
  EXPECT_EQ(one + one, two);
  EXPECT_EQ(big_minus_one + one, big);
  EXPECT_EQ(one - one, zero);
  EXPECT_EQ(one - zero, one);
  EXPECT_EQ(zero - one, biggest);
  EXPECT_EQ(big - big, zero);
  EXPECT_EQ(big - one, big_minus_one);
  EXPECT_EQ(big + std::numeric_limits<uint64_t>::max(), bigger);
  EXPECT_EQ(biggest + 1, zero);
  EXPECT_EQ(zero - 1, biggest);
  EXPECT_EQ(high_low - one, low_high);
  EXPECT_EQ(low_high + one, high_low);
  EXPECT_EQ(absl::Uint128High64((absl::uint128(1) << 64) - 1), 0);
  EXPECT_EQ(absl::Uint128Low64((absl::uint128(1) << 64) - 1),
            std::numeric_limits<uint64_t>::max());
  EXPECT_TRUE(!!one);
  EXPECT_TRUE(!!high_low);
  EXPECT_FALSE(!!zero);
  EXPECT_FALSE(!one);
  EXPECT_FALSE(!high_low);
  EXPECT_TRUE(!zero);
  EXPECT_TRUE(zero == 0);       // NOLINT(readability/check)
  EXPECT_FALSE(zero != 0);      // NOLINT(readability/check)
  EXPECT_FALSE(one == 0);       // NOLINT(readability/check)
  EXPECT_TRUE(one != 0);        // NOLINT(readability/check)
  EXPECT_FALSE(high_low == 0);  // NOLINT(readability/check)
  EXPECT_TRUE(high_low != 0);   // NOLINT(readability/check)

  absl::uint128 test = zero;
  EXPECT_EQ(++test, one);
  EXPECT_EQ(test, one);
  EXPECT_EQ(test++, one);
  EXPECT_EQ(test, two);
  EXPECT_EQ(test -= 2, zero);
  EXPECT_EQ(test, zero);
  EXPECT_EQ(test += 2, two);
  EXPECT_EQ(test, two);
  EXPECT_EQ(--test, one);
  EXPECT_EQ(test, one);
  EXPECT_EQ(test--, one);
  EXPECT_EQ(test, zero);
  EXPECT_EQ(test |= three, three);
  EXPECT_EQ(test &= one, one);
  EXPECT_EQ(test ^= three, two);
  EXPECT_EQ(test >>= 1, one);
  EXPECT_EQ(test <<= 1, two);

  EXPECT_EQ(big, -(-big));
  EXPECT_EQ(two, -((-one) - 1));
  EXPECT_EQ(absl::Uint128Max(), -one);
  EXPECT_EQ(zero, -zero);

  EXPECT_EQ(absl::Uint128Max(), absl::kuint128max);
}

TEST(Uint128, ConversionTests) {
  EXPECT_TRUE(absl::MakeUint128(1, 0));

#ifdef ABSL_HAVE_INTRINSIC_INT128
  unsigned __int128 intrinsic =
      (static_cast<unsigned __int128>(0x3a5b76c209de76f6) << 64) +
      0x1f25e1d63a2b46c5;
  absl::uint128 custom =
      absl::MakeUint128(0x3a5b76c209de76f6, 0x1f25e1d63a2b46c5);

  EXPECT_EQ(custom, absl::uint128(intrinsic));
  EXPECT_EQ(custom, absl::uint128(static_cast<__int128>(intrinsic)));
  EXPECT_EQ(intrinsic, static_cast<unsigned __int128>(custom));
  EXPECT_EQ(intrinsic, static_cast<__int128>(custom));
#endif  // ABSL_HAVE_INTRINSIC_INT128

  // verify that an integer greater than 2**64 that can be stored precisely
  // inside a double is converted to a absl::uint128 without loss of
  // information.
  double precise_double = 0x530e * std::pow(2.0, 64.0) + 0xda74000000000000;
  absl::uint128 from_precise_double(precise_double);
  absl::uint128 from_precise_ints =
      absl::MakeUint128(0x530e, 0xda74000000000000);
  EXPECT_EQ(from_precise_double, from_precise_ints);
  EXPECT_DOUBLE_EQ(static_cast<double>(from_precise_ints), precise_double);

  double approx_double = 0xffffeeeeddddcccc * std::pow(2.0, 64.0) +
                         0xbbbbaaaa99998888;
  absl::uint128 from_approx_double(approx_double);
  EXPECT_DOUBLE_EQ(static_cast<double>(from_approx_double), approx_double);

  double round_to_zero = 0.7;
  double round_to_five = 5.8;
  double round_to_nine = 9.3;
  EXPECT_EQ(static_cast<absl::uint128>(round_to_zero), 0);
  EXPECT_EQ(static_cast<absl::uint128>(round_to_five), 5);
  EXPECT_EQ(static_cast<absl::uint128>(round_to_nine), 9);
}

TEST(Uint128, OperatorAssignReturnRef) {
  absl::uint128 v(1);
  (v += 4) -= 3;
  EXPECT_EQ(2, v);
}

TEST(Uint128, Multiply) {
  absl::uint128 a, b, c;

  // Zero test.
  a = 0;
  b = 0;
  c = a * b;
  EXPECT_EQ(0, c);

  // Max carries.
  a = absl::uint128(0) - 1;
  b = absl::uint128(0) - 1;
  c = a * b;
  EXPECT_EQ(1, c);

  // Self-operation with max carries.
  c = absl::uint128(0) - 1;
  c *= c;
  EXPECT_EQ(1, c);

  // 1-bit x 1-bit.
  for (int i = 0; i < 64; ++i) {
    for (int j = 0; j < 64; ++j) {
      a = absl::uint128(1) << i;
      b = absl::uint128(1) << j;
      c = a * b;
      EXPECT_EQ(absl::uint128(1) << (i + j), c);
    }
  }

  // Verified with dc.
  a = absl::MakeUint128(0xffffeeeeddddcccc, 0xbbbbaaaa99998888);
  b = absl::MakeUint128(0x7777666655554444, 0x3333222211110000);
  c = a * b;
  EXPECT_EQ(absl::MakeUint128(0x530EDA741C71D4C3, 0xBF25975319080000), c);
  EXPECT_EQ(0, c - b * a);
  EXPECT_EQ(a*a - b*b, (a+b) * (a-b));

  // Verified with dc.
  a = absl::MakeUint128(0x0123456789abcdef, 0xfedcba9876543210);
  b = absl::MakeUint128(0x02468ace13579bdf, 0xfdb97531eca86420);
  c = a * b;
  EXPECT_EQ(absl::MakeUint128(0x97a87f4f261ba3f2, 0x342d0bbf48948200), c);
  EXPECT_EQ(0, c - b * a);
  EXPECT_EQ(a*a - b*b, (a+b) * (a-b));
}

TEST(Uint128, AliasTests) {
  absl::uint128 x1 = absl::MakeUint128(1, 2);
  absl::uint128 x2 = absl::MakeUint128(2, 4);
  x1 += x1;
  EXPECT_EQ(x2, x1);

  absl::uint128 x3 = absl::MakeUint128(1, static_cast<uint64_t>(1) << 63);
  absl::uint128 x4 = absl::MakeUint128(3, 0);
  x3 += x3;
  EXPECT_EQ(x4, x3);
}

TEST(Uint128, DivideAndMod) {
  using std::swap;

  // a := q * b + r
  absl::uint128 a, b, q, r;

  // Zero test.
  a = 0;
  b = 123;
  q = a / b;
  r = a % b;
  EXPECT_EQ(0, q);
  EXPECT_EQ(0, r);

  a = absl::MakeUint128(0x530eda741c71d4c3, 0xbf25975319080000);
  q = absl::MakeUint128(0x4de2cab081, 0x14c34ab4676e4bab);
  b = absl::uint128(0x1110001);
  r = absl::uint128(0x3eb455);
  ASSERT_EQ(a, q * b + r);  // Sanity-check.

  absl::uint128 result_q, result_r;
  result_q = a / b;
  result_r = a % b;
  EXPECT_EQ(q, result_q);
  EXPECT_EQ(r, result_r);

  // Try the other way around.
  swap(q, b);
  result_q = a / b;
  result_r = a % b;
  EXPECT_EQ(q, result_q);
  EXPECT_EQ(r, result_r);
  // Restore.
  swap(b, q);

  // Dividend < divisor; result should be q:0 r:<dividend>.
  swap(a, b);
  result_q = a / b;
  result_r = a % b;
  EXPECT_EQ(0, result_q);
  EXPECT_EQ(a, result_r);
  // Try the other way around.
  swap(a, q);
  result_q = a / b;
  result_r = a % b;
  EXPECT_EQ(0, result_q);
  EXPECT_EQ(a, result_r);
  // Restore.
  swap(q, a);
  swap(b, a);

  // Try a large remainder.
  b = a / 2 + 1;
  absl::uint128 expected_r =
      absl::MakeUint128(0x29876d3a0e38ea61, 0xdf92cba98c83ffff);
  // Sanity checks.
  ASSERT_EQ(a / 2 - 1, expected_r);
  ASSERT_EQ(a, b + expected_r);
  result_q = a / b;
  result_r = a % b;
  EXPECT_EQ(1, result_q);
  EXPECT_EQ(expected_r, result_r);
}

TEST(Uint128, DivideAndModRandomInputs) {
  const int kNumIters = 1 << 18;
  std::minstd_rand random(testing::UnitTest::GetInstance()->random_seed());
  std::uniform_int_distribution<uint64_t> uniform_uint64;
  for (int i = 0; i < kNumIters; ++i) {
    const absl::uint128 a =
        absl::MakeUint128(uniform_uint64(random), uniform_uint64(random));
    const absl::uint128 b =
        absl::MakeUint128(uniform_uint64(random), uniform_uint64(random));
    if (b == 0) {
      continue;  // Avoid a div-by-zero.
    }
    const absl::uint128 q = a / b;
    const absl::uint128 r = a % b;
    ASSERT_EQ(a, b * q + r);
  }
}

TEST(Uint128, ConstexprTest) {
  constexpr absl::uint128 zero = absl::uint128();
  constexpr absl::uint128 one = 1;
  constexpr absl::uint128 minus_two = -2;
  EXPECT_EQ(zero, absl::uint128(0));
  EXPECT_EQ(one, absl::uint128(1));
  EXPECT_EQ(minus_two, absl::MakeUint128(-1, -2));
}

TEST(Uint128, NumericLimitsTest) {
  static_assert(std::numeric_limits<absl::uint128>::is_specialized, "");
  static_assert(!std::numeric_limits<absl::uint128>::is_signed, "");
  static_assert(std::numeric_limits<absl::uint128>::is_integer, "");
  EXPECT_EQ(static_cast<int>(128 * std::log10(2)),
            std::numeric_limits<absl::uint128>::digits10);
  EXPECT_EQ(0, std::numeric_limits<absl::uint128>::min());
  EXPECT_EQ(0, std::numeric_limits<absl::uint128>::lowest());
  EXPECT_EQ(absl::Uint128Max(), std::numeric_limits<absl::uint128>::max());
}

}  // namespace
