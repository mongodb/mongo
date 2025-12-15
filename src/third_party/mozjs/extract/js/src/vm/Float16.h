/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Float16_h
#define vm_Float16_h

#include "mozilla/FloatingPoint.h"
#include "mozilla/MathAlgorithms.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace js {

namespace half {
// This is extracted from Version 2.2.0 of the half library by Christian Rau.
// See https://sourceforge.net/projects/half/.
// The original copyright and MIT license are reproduced below:

// half - IEEE 754-based half-precision floating-point library.
//
// Copyright (c) 2012-2021 Christian Rau <rauy@users.sourceforge.net>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

/// Type traits for floating-point bits.
template <typename T>
struct bits {
  typedef unsigned char type;
};
template <typename T>
struct bits<const T> : bits<T> {};
template <typename T>
struct bits<volatile T> : bits<T> {};
template <typename T>
struct bits<const volatile T> : bits<T> {};

/// Unsigned integer of (at least) 32 bits width.
template <>
struct bits<float> {
  typedef std::uint_least32_t type;
};

/// Unsigned integer of (at least) 64 bits width.
template <>
struct bits<double> {
  typedef std::uint_least64_t type;
};

/// Fastest unsigned integer of (at least) 32 bits width.
typedef std::uint_fast32_t uint32;

/// Half-precision overflow.
/// \param sign half-precision value with sign bit only
/// \return rounded overflowing half-precision value
constexpr unsigned int overflow(unsigned int sign = 0) { return sign | 0x7C00; }

/// Half-precision underflow.
/// \param sign half-precision value with sign bit only
/// \return rounded underflowing half-precision value
constexpr unsigned int underflow(unsigned int sign = 0) { return sign; }

/// Round half-precision number.
/// \param value finite half-precision number to round
/// \param g guard bit (most significant discarded bit)
/// \param s sticky bit (or of all but the most significant discarded bits)
/// \return rounded half-precision value
constexpr unsigned int rounded(unsigned int value, int g, int s) {
  return value + (g & (s | value));
}

/// Convert IEEE single-precision to half-precision.
/// \param value single-precision value to convert
/// \return rounded half-precision value
inline unsigned int float2half_impl(float value) {
  bits<float>::type fbits;
  std::memcpy(&fbits, &value, sizeof(float));
  unsigned int sign = (fbits >> 16) & 0x8000;
  fbits &= 0x7FFFFFFF;
  if (fbits >= 0x7F800000)
    return sign | 0x7C00 |
           ((fbits > 0x7F800000) ? (0x200 | ((fbits >> 13) & 0x3FF)) : 0);
  if (fbits >= 0x47800000) return overflow(sign);
  if (fbits >= 0x38800000)
    return rounded(
        sign | (((fbits >> 23) - 112) << 10) | ((fbits >> 13) & 0x3FF),
        (fbits >> 12) & 1, (fbits & 0xFFF) != 0);
  if (fbits >= 0x33000000) {
    int i = 125 - (fbits >> 23);
    fbits = (fbits & 0x7FFFFF) | 0x800000;
    return rounded(sign | (fbits >> (i + 1)), (fbits >> i) & 1,
                   (fbits & ((static_cast<uint32>(1) << i) - 1)) != 0);
  }
  if (fbits != 0) return underflow(sign);
  return sign;
}

/// Convert IEEE double-precision to half-precision.
/// \param value double-precision value to convert
/// \return rounded half-precision value
inline unsigned int float2half_impl(double value) {
  bits<double>::type dbits;
  std::memcpy(&dbits, &value, sizeof(double));
  uint32 hi = dbits >> 32, lo = dbits & 0xFFFFFFFF;
  unsigned int sign = (hi >> 16) & 0x8000;
  hi &= 0x7FFFFFFF;
  if (hi >= 0x7FF00000)
    return sign | 0x7C00 |
           ((dbits & 0xFFFFFFFFFFFFF) ? (0x200 | ((hi >> 10) & 0x3FF)) : 0);
  if (hi >= 0x40F00000) return overflow(sign);
  if (hi >= 0x3F100000)
    return rounded(sign | (((hi >> 20) - 1008) << 10) | ((hi >> 10) & 0x3FF),
                   (hi >> 9) & 1, ((hi & 0x1FF) | lo) != 0);
  if (hi >= 0x3E600000) {
    int i = 1018 - (hi >> 20);
    hi = (hi & 0xFFFFF) | 0x100000;
    return rounded(sign | (hi >> (i + 1)), (hi >> i) & 1,
                   ((hi & ((static_cast<uint32>(1) << i) - 1)) | lo) != 0);
  }
  if ((hi | lo) != 0) return underflow(sign);
  return sign;
}

template <typename T>
inline T half2float_impl(unsigned int value);

/// Convert half-precision to IEEE double-precision.
/// \param value half-precision value to convert
/// \return double-precision value
template <>
inline double half2float_impl(unsigned int value) {
  uint32 hi = static_cast<uint32>(value & 0x8000) << 16;
  unsigned int abs = value & 0x7FFF;
  if (abs) {
    hi |= 0x3F000000 << static_cast<unsigned>(abs >= 0x7C00);

    // Mozilla change: Replace the loop with CountLeadingZeroes32.
    // for (; abs < 0x400; abs <<= 1, hi -= 0x100000);
    if (abs < 0x400) {
      // NOTE: CountLeadingZeroes32(0x400) is 21.
      uint32 shift = mozilla::CountLeadingZeroes32(uint32_t(abs)) - 21;
      abs <<= shift;
      hi -= shift * 0x100000;
    }

    hi += static_cast<uint32>(abs) << 10;
  }
  bits<double>::type dbits = static_cast<bits<double>::type>(hi) << 32;
  double out;
  std::memcpy(&out, &dbits, sizeof(double));
  return out;
}

/// Convert half-precision to IEEE single-precision.
/// \param value half-precision value to convert
/// \return single-precision value
template <>
inline float half2float_impl(unsigned int value) {
  bits<float>::type fbits = static_cast<bits<float>::type>(value & 0x8000)
                            << 16;
  unsigned int abs = value & 0x7FFF;
  if (abs) {
    fbits |= 0x38000000 << static_cast<unsigned>(abs >= 0x7C00);

    // Mozilla change: Replace the loop with CountLeadingZeroes32.
    // for (; abs < 0x400; abs <<= 1, fbits -= 0x800000);
    if (abs < 0x400) {
      // NOTE: CountLeadingZeroes32(0x400) is 21.
      uint32 shift = mozilla::CountLeadingZeroes32(uint32_t(abs)) - 21;
      abs <<= shift;
      fbits -= shift * 0x800000;
    }

    fbits += static_cast<bits<float>::type>(abs) << 13;
  }

  float out;
  std::memcpy(&out, &fbits, sizeof(float));
  return out;
}
}  // namespace half

class float16 final {
  uint16_t val;

 public:
  // The default constructor can be 'constexpr' when we switch to C++20.
  //
  // C++17 requires explicit initialization of all members when using a
  // 'constexpr' default constructor. That means `val` needs to be initialized
  // through a member initializer. But adding a member initializer makes the
  // class no longer trivial, which breaks memcpy/memset optimizations.

  /* constexpr */ float16() = default;
  constexpr float16(const float16&) = default;

  explicit float16(float x) : val(half::float2half_impl(x)) {}
  explicit float16(double x) : val(half::float2half_impl(x)) {}

  explicit float16(std::int8_t x) : float16(float(x)) {}
  explicit float16(std::int16_t x) : float16(float(x)) {}
  explicit float16(std::int32_t x) : float16(float(x)) {}
  explicit float16(std::int64_t x) : float16(double(x)) {}

  explicit float16(std::uint8_t x) : float16(float(x)) {}
  explicit float16(std::uint16_t x) : float16(float(x)) {}
  explicit float16(std::uint32_t x) : float16(float(x)) {}
  explicit float16(std::uint64_t x) : float16(double(x)) {}

  explicit float16(bool x) : float16(float(x)) {}

  constexpr float16& operator=(const float16&) = default;

  float16& operator=(float x) {
    *this = float16{x};
    return *this;
  }

  float16& operator=(double x) {
    *this = float16{x};
    return *this;
  }

  explicit operator float() const { return half::half2float_impl<float>(val); }
  explicit operator double() const {
    return half::half2float_impl<double>(val);
  }

  bool operator==(float16 x) const {
    uint16_t abs = val & 0x7FFF;

    // ±0 is equal to ±0.
    if (abs == 0) {
      return (x.val & 0x7FFF) == 0;
    }

    // If neither +0 nor NaN, then both bit representations must be equal.
    if (abs <= 0x7C00) {
      return val == x.val;
    }

    // NaN isn't equal to any value.
    return false;
  }

  bool operator!=(float16 x) const { return !(*this == x); }

  uint16_t toRawBits() const { return val; }

  static constexpr float16 fromRawBits(uint16_t bits) {
    float16 f16{};
    f16.val = bits;
    return f16;
  }
};

static_assert(sizeof(float16) == 2, "float16 has no extra padding");

static_assert(
    std::is_trivial_v<float16>,
    "float16 must be trivial to be eligible for memcpy/memset optimizations");

}  // namespace js

template <>
class std::numeric_limits<js::float16> {
 public:
  static constexpr bool is_specialized = true;
  static constexpr bool is_signed = true;
  static constexpr bool is_integer = false;
  static constexpr bool is_exact = false;
  static constexpr bool has_infinity = true;
  static constexpr bool has_quiet_NaN = true;
  static constexpr bool has_signaling_NaN = true;
  static constexpr std::float_denorm_style has_denorm = std::denorm_present;
  static constexpr bool has_denorm_loss = false;
  static constexpr std::float_round_style round_style = std::round_to_nearest;
  static constexpr bool is_iec559 = true;
  static constexpr bool is_bounded = true;
  static constexpr bool is_modulo = false;
  static constexpr int digits = 11;
  static constexpr int digits10 = 3;
  static constexpr int max_digits10 = 5;
  static constexpr int radix = 2;
  static constexpr int min_exponent = -13;
  static constexpr int min_exponent10 = -4;
  static constexpr int max_exponent = 16;
  static constexpr int max_exponent10 = 4;
  static constexpr bool traps = false;
  static constexpr bool tinyness_before = false;

  static constexpr auto min() noexcept {
    return js::float16::fromRawBits(0x400);
  }
  static constexpr auto lowest() noexcept {
    return js::float16::fromRawBits(0xFBFF);
  }
  static constexpr auto max() noexcept {
    return js::float16::fromRawBits(0x7BFF);
  }
  static constexpr auto epsilon() noexcept {
    return js::float16::fromRawBits(0x1400);
  }
  static constexpr auto round_error() noexcept {
    return js::float16::fromRawBits(0x3800);
  }
  static constexpr auto infinity() noexcept {
    return js::float16::fromRawBits(0x7C00);
  }
  static constexpr auto quiet_NaN() noexcept {
    return js::float16::fromRawBits(0x7E00);
  }
  static constexpr auto signaling_NaN() noexcept {
    return js::float16::fromRawBits(0x7D00);
  }
  static constexpr auto denorm_min() noexcept {
    return js::float16::fromRawBits(0x0001);
  }
};

template <>
struct mozilla::detail::FloatingPointTrait<js::float16> {
 protected:
  using Bits = uint16_t;

  static constexpr unsigned kExponentWidth = 5;
  static constexpr unsigned kSignificandWidth = 10;
};

#endif  // vm_Float16_h
