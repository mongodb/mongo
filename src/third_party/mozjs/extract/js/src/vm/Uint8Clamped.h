/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Uint8Clamped_h
#define vm_Uint8Clamped_h

#include <stdint.h>

namespace js {

extern uint32_t ClampDoubleToUint8(const double x);

struct uint8_clamped {
  uint8_t val;

  uint8_clamped() = default;
  uint8_clamped(const uint8_clamped& other) = default;

  // invoke our assignment helpers for constructor conversion
  explicit uint8_clamped(uint8_t x) { *this = x; }
  explicit uint8_clamped(uint16_t x) { *this = x; }
  explicit uint8_clamped(uint32_t x) { *this = x; }
  explicit uint8_clamped(uint64_t x) { *this = x; }
  explicit uint8_clamped(int8_t x) { *this = x; }
  explicit uint8_clamped(int16_t x) { *this = x; }
  explicit uint8_clamped(int32_t x) { *this = x; }
  explicit uint8_clamped(int64_t x) { *this = x; }
  explicit uint8_clamped(double x) { *this = x; }

  uint8_clamped& operator=(const uint8_clamped& x) = default;

  uint8_clamped& operator=(uint8_t x) {
    val = x;
    return *this;
  }

  uint8_clamped& operator=(uint16_t x) {
    val = (x > 255) ? 255 : uint8_t(x);
    return *this;
  }

  uint8_clamped& operator=(uint32_t x) {
    val = (x > 255) ? 255 : uint8_t(x);
    return *this;
  }

  uint8_clamped& operator=(uint64_t x) {
    val = (x > 255) ? 255 : uint8_t(x);
    return *this;
  }

  uint8_clamped& operator=(int8_t x) {
    val = (x >= 0) ? uint8_t(x) : 0;
    return *this;
  }

  uint8_clamped& operator=(int16_t x) {
    val = (x >= 0) ? ((x < 255) ? uint8_t(x) : 255) : 0;
    return *this;
  }

  uint8_clamped& operator=(int32_t x) {
    val = (x >= 0) ? ((x < 255) ? uint8_t(x) : 255) : 0;
    return *this;
  }

  uint8_clamped& operator=(int64_t x) {
    val = (x >= 0) ? ((x < 255) ? uint8_t(x) : 255) : 0;
    return *this;
  }

  uint8_clamped& operator=(const double x) {
    val = uint8_t(ClampDoubleToUint8(x));
    return *this;
  }

  operator uint8_t() const { return val; }

  void staticAsserts() {
    static_assert(sizeof(uint8_clamped) == 1,
                  "uint8_clamped must be layout-compatible with uint8_t");
  }
};

/* Note that we can't use std::numeric_limits here due to uint8_clamped. */
template <typename T>
inline constexpr bool TypeIsFloatingPoint() {
  return false;
}
template <>
inline constexpr bool TypeIsFloatingPoint<float>() {
  return true;
}
template <>
inline constexpr bool TypeIsFloatingPoint<double>() {
  return true;
}

template <typename T>
inline constexpr bool TypeIsUnsigned() {
  return false;
}
template <>
inline constexpr bool TypeIsUnsigned<uint8_t>() {
  return true;
}
template <>
inline constexpr bool TypeIsUnsigned<uint16_t>() {
  return true;
}
template <>
inline constexpr bool TypeIsUnsigned<uint32_t>() {
  return true;
}

}  // namespace js

#endif  // vm_Uint8Clamped_h
