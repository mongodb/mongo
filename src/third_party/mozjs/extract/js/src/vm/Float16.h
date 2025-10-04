/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Float16_h
#define vm_Float16_h

#include <cstdint>
#include <cstring>
#include <limits>

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

/// Convert half-precision to IEEE double-precision.
/// \param value half-precision value to convert
/// \return double-precision value
inline double half2float_impl(unsigned int value) {
  uint32 hi = static_cast<uint32>(value & 0x8000) << 16;
  unsigned int abs = value & 0x7FFF;
  if (abs) {
    hi |= 0x3F000000 << static_cast<unsigned>(abs >= 0x7C00);
    for (; abs < 0x400; abs <<= 1, hi -= 0x100000)
      ;
    hi += static_cast<uint32>(abs) << 10;
  }
  bits<double>::type dbits = static_cast<bits<double>::type>(hi) << 32;
  double out;
  std::memcpy(&out, &dbits, sizeof(double));
  return out;
}
}  // namespace half

struct float16 {
  uint16_t val;

  float16() = default;
  float16(const float16& other) = default;

  explicit float16(double x) { *this = x; }

  float16& operator=(const float16& x) = default;

  float16& operator=(double x) {
    this->val = half::float2half_impl(x);
    return *this;
  }

  double toDouble() { return half::half2float_impl(this->val); }
};

static_assert(sizeof(float16) == 2, "float16 has no extra padding");

}  // namespace js

#endif  // vm_Float16_h
