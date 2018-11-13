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

#include <stddef.h>
#include <cassert>
#include <iomanip>
#include <ostream>  // NOLINT(readability/streams)
#include <sstream>
#include <string>
#include <type_traits>

namespace absl {

const uint128 kuint128max = MakeUint128(std::numeric_limits<uint64_t>::max(),
                                        std::numeric_limits<uint64_t>::max());

namespace {

// Returns the 0-based position of the last set bit (i.e., most significant bit)
// in the given uint64_t. The argument may not be 0.
//
// For example:
//   Given: 5 (decimal) == 101 (binary)
//   Returns: 2
#define STEP(T, n, pos, sh)                   \
  do {                                        \
    if ((n) >= (static_cast<T>(1) << (sh))) { \
      (n) = (n) >> (sh);                      \
      (pos) |= (sh);                          \
    }                                         \
  } while (0)
static inline int Fls64(uint64_t n) {
  assert(n != 0);
  int pos = 0;
  STEP(uint64_t, n, pos, 0x20);
  uint32_t n32 = static_cast<uint32_t>(n);
  STEP(uint32_t, n32, pos, 0x10);
  STEP(uint32_t, n32, pos, 0x08);
  STEP(uint32_t, n32, pos, 0x04);
  return pos + ((uint64_t{0x3333333322221100} >> (n32 << 2)) & 0x3);
}
#undef STEP

// Like Fls64() above, but returns the 0-based position of the last set bit
// (i.e., most significant bit) in the given uint128. The argument may not be 0.
static inline int Fls128(uint128 n) {
  if (uint64_t hi = Uint128High64(n)) {
    return Fls64(hi) + 64;
  }
  return Fls64(Uint128Low64(n));
}

// Long division/modulo for uint128 implemented using the shift-subtract
// division algorithm adapted from:
// http://stackoverflow.com/questions/5386377/division-without-using
void DivModImpl(uint128 dividend, uint128 divisor, uint128* quotient_ret,
                uint128* remainder_ret) {
  assert(divisor != 0);

  if (divisor > dividend) {
    *quotient_ret = 0;
    *remainder_ret = dividend;
    return;
  }

  if (divisor == dividend) {
    *quotient_ret = 1;
    *remainder_ret = 0;
    return;
  }

  uint128 denominator = divisor;
  uint128 quotient = 0;

  // Left aligns the MSB of the denominator and the dividend.
  const int shift = Fls128(dividend) - Fls128(denominator);
  denominator <<= shift;

  // Uses shift-subtract algorithm to divide dividend by denominator. The
  // remainder will be left in dividend.
  for (int i = 0; i <= shift; ++i) {
    quotient <<= 1;
    if (dividend >= denominator) {
      dividend -= denominator;
      quotient |= 1;
    }
    denominator >>= 1;
  }

  *quotient_ret = quotient;
  *remainder_ret = dividend;
}

template <typename T>
uint128 MakeUint128FromFloat(T v) {
  static_assert(std::is_floating_point<T>::value, "");

  // Rounding behavior is towards zero, same as for built-in types.

  // Undefined behavior if v is NaN or cannot fit into uint128.
  assert(std::isfinite(v) && v > -1 &&
         (std::numeric_limits<T>::max_exponent <= 128 ||
          v < std::ldexp(static_cast<T>(1), 128)));

  if (v >= std::ldexp(static_cast<T>(1), 64)) {
    uint64_t hi = static_cast<uint64_t>(std::ldexp(v, -64));
    uint64_t lo = static_cast<uint64_t>(v - std::ldexp(static_cast<T>(hi), 64));
    return MakeUint128(hi, lo);
  }

  return MakeUint128(0, static_cast<uint64_t>(v));
}
}  // namespace

uint128::uint128(float v) : uint128(MakeUint128FromFloat(v)) {}
uint128::uint128(double v) : uint128(MakeUint128FromFloat(v)) {}
uint128::uint128(long double v) : uint128(MakeUint128FromFloat(v)) {}

uint128 operator/(uint128 lhs, uint128 rhs) {
#if defined(ABSL_HAVE_INTRINSIC_INT128)
  return static_cast<unsigned __int128>(lhs) /
         static_cast<unsigned __int128>(rhs);
#else  // ABSL_HAVE_INTRINSIC_INT128
  uint128 quotient = 0;
  uint128 remainder = 0;
  DivModImpl(lhs, rhs, &quotient, &remainder);
  return quotient;
#endif  // ABSL_HAVE_INTRINSIC_INT128
}
uint128 operator%(uint128 lhs, uint128 rhs) {
#if defined(ABSL_HAVE_INTRINSIC_INT128)
  return static_cast<unsigned __int128>(lhs) %
         static_cast<unsigned __int128>(rhs);
#else  // ABSL_HAVE_INTRINSIC_INT128
  uint128 quotient = 0;
  uint128 remainder = 0;
  DivModImpl(lhs, rhs, &quotient, &remainder);
  return remainder;
#endif  // ABSL_HAVE_INTRINSIC_INT128
}

namespace {

std::string Uint128ToFormattedString(uint128 v, std::ios_base::fmtflags flags) {
  // Select a divisor which is the largest power of the base < 2^64.
  uint128 div;
  int div_base_log;
  switch (flags & std::ios::basefield) {
    case std::ios::hex:
      div = 0x1000000000000000;  // 16^15
      div_base_log = 15;
      break;
    case std::ios::oct:
      div = 01000000000000000000000;  // 8^21
      div_base_log = 21;
      break;
    default:  // std::ios::dec
      div = 10000000000000000000u;  // 10^19
      div_base_log = 19;
      break;
  }

  // Now piece together the uint128 representation from three chunks of the
  // original value, each less than "div" and therefore representable as a
  // uint64_t.
  std::ostringstream os;
  std::ios_base::fmtflags copy_mask =
      std::ios::basefield | std::ios::showbase | std::ios::uppercase;
  os.setf(flags & copy_mask, copy_mask);
  uint128 high = v;
  uint128 low;
  DivModImpl(high, div, &high, &low);
  uint128 mid;
  DivModImpl(high, div, &high, &mid);
  if (Uint128Low64(high) != 0) {
    os << Uint128Low64(high);
    os << std::noshowbase << std::setfill('0') << std::setw(div_base_log);
    os << Uint128Low64(mid);
    os << std::setw(div_base_log);
  } else if (Uint128Low64(mid) != 0) {
    os << Uint128Low64(mid);
    os << std::noshowbase << std::setfill('0') << std::setw(div_base_log);
  }
  os << Uint128Low64(low);
  return os.str();
}

}  // namespace

std::ostream& operator<<(std::ostream& os, uint128 v) {
  std::ios_base::fmtflags flags = os.flags();
  std::string rep = Uint128ToFormattedString(v, flags);

  // Add the requisite padding.
  std::streamsize width = os.width(0);
  if (static_cast<size_t>(width) > rep.size()) {
    std::ios::fmtflags adjustfield = flags & std::ios::adjustfield;
    if (adjustfield == std::ios::left) {
      rep.append(width - rep.size(), os.fill());
    } else if (adjustfield == std::ios::internal &&
               (flags & std::ios::showbase) &&
               (flags & std::ios::basefield) == std::ios::hex && v != 0) {
      rep.insert(2, width - rep.size(), os.fill());
    } else {
      rep.insert(0, width - rep.size(), os.fill());
    }
  }

  return os << rep;
}

}  // namespace absl

namespace std {
constexpr bool numeric_limits<absl::uint128>::is_specialized;
constexpr bool numeric_limits<absl::uint128>::is_signed;
constexpr bool numeric_limits<absl::uint128>::is_integer;
constexpr bool numeric_limits<absl::uint128>::is_exact;
constexpr bool numeric_limits<absl::uint128>::has_infinity;
constexpr bool numeric_limits<absl::uint128>::has_quiet_NaN;
constexpr bool numeric_limits<absl::uint128>::has_signaling_NaN;
constexpr float_denorm_style numeric_limits<absl::uint128>::has_denorm;
constexpr bool numeric_limits<absl::uint128>::has_denorm_loss;
constexpr float_round_style numeric_limits<absl::uint128>::round_style;
constexpr bool numeric_limits<absl::uint128>::is_iec559;
constexpr bool numeric_limits<absl::uint128>::is_bounded;
constexpr bool numeric_limits<absl::uint128>::is_modulo;
constexpr int numeric_limits<absl::uint128>::digits;
constexpr int numeric_limits<absl::uint128>::digits10;
constexpr int numeric_limits<absl::uint128>::max_digits10;
constexpr int numeric_limits<absl::uint128>::radix;
constexpr int numeric_limits<absl::uint128>::min_exponent;
constexpr int numeric_limits<absl::uint128>::min_exponent10;
constexpr int numeric_limits<absl::uint128>::max_exponent;
constexpr int numeric_limits<absl::uint128>::max_exponent10;
constexpr bool numeric_limits<absl::uint128>::traps;
constexpr bool numeric_limits<absl::uint128>::tinyness_before;
}  // namespace std
