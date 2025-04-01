/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Cast operations to supplement the built-in casting operations. */

#ifndef mozilla_Casting_h
#define mozilla_Casting_h

#include "mozilla/Assertions.h"

#include <cstring>
#include <type_traits>
#include <limits>
#include <cmath>

namespace mozilla {

/**
 * Sets the outparam value of type |To| with the same underlying bit pattern of
 * |aFrom|.
 *
 * |To| and |From| must be types of the same size; be careful of cross-platform
 * size differences, or this might fail to compile on some but not all
 * platforms.
 *
 * There is also a variant that returns the value directly.  In most cases, the
 * two variants should be identical.  However, in the specific case of x86
 * chips, the behavior differs: returning floating-point values directly is done
 * through the x87 stack, and x87 loads and stores turn signaling NaNs into
 * quiet NaNs... silently.  Returning floating-point values via outparam,
 * however, is done entirely within the SSE registers when SSE2 floating-point
 * is enabled in the compiler, which has semantics-preserving behavior you would
 * expect.
 *
 * If preserving the distinction between signaling NaNs and quiet NaNs is
 * important to you, you should use the outparam version.  In all other cases,
 * you should use the direct return version.
 */
template <typename To, typename From>
inline void BitwiseCast(const From aFrom, To* aResult) {
  static_assert(sizeof(From) == sizeof(To),
                "To and From must have the same size");

  // We could maybe downgrade these to std::is_trivially_copyable, but the
  // various STLs we use don't all provide it.
  static_assert(std::is_trivial<From>::value,
                "shouldn't bitwise-copy a type having non-trivial "
                "initialization");
  static_assert(std::is_trivial<To>::value,
                "shouldn't bitwise-copy a type having non-trivial "
                "initialization");

  std::memcpy(static_cast<void*>(aResult), static_cast<const void*>(&aFrom),
              sizeof(From));
}

template <typename To, typename From>
inline To BitwiseCast(const From aFrom) {
  To temp;
  BitwiseCast<To, From>(aFrom, &temp);
  return temp;
}

namespace detail {

template <typename T>
constexpr int64_t safe_integer() {
  static_assert(std::is_floating_point_v<T>);
  return std::pow(2, std::numeric_limits<T>::digits);
}

template <typename T>
constexpr uint64_t safe_integer_unsigned() {
  static_assert(std::is_floating_point_v<T>);
  return std::pow(2, std::numeric_limits<T>::digits);
}

// This is working around https://gcc.gnu.org/bugzilla/show_bug.cgi?id=81676,
// fixed in gcc-10
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
template <typename In, typename Out>
bool IsInBounds(In aIn) {
  constexpr bool inSigned = std::is_signed_v<In>;
  constexpr bool outSigned = std::is_signed_v<Out>;
  constexpr bool bothSigned = inSigned && outSigned;
  constexpr bool bothUnsigned = !inSigned && !outSigned;
  constexpr bool inFloat = std::is_floating_point_v<In>;
  constexpr bool outFloat = std::is_floating_point_v<Out>;
  constexpr bool bothFloat = inFloat && outFloat;
  constexpr bool noneFloat = !inFloat && !outFloat;
  constexpr Out outMax = std::numeric_limits<Out>::max();
  constexpr Out outMin = std::numeric_limits<Out>::lowest();

  // This selects the widest of two types, and is used to cast throughout.
  using select_widest = std::conditional_t<(sizeof(In) > sizeof(Out)), In, Out>;

  if constexpr (bothFloat) {
    if (aIn > select_widest(outMax) || aIn < select_widest(outMin)) {
      return false;
    }
  }
  // Normal casting applies, the floating point number is floored.
  if constexpr (inFloat && !outFloat) {
    static_assert(sizeof(aIn) <= sizeof(int64_t));
    // Check if the input floating point is larger than the output bounds. This
    // catches situations where the input is a float larger than the max of the
    // output type.
    if (aIn < static_cast<double>(outMin) ||
        aIn > static_cast<double>(outMax)) {
      return false;
    }
    // At this point we know that the input can be converted to an integer.
    // Check if it's larger than the bounds of the target integer.
    if (outSigned) {
      int64_t asInteger = static_cast<int64_t>(aIn);
      if (asInteger < outMin || asInteger > outMax) {
        return false;
      }
    } else {
      uint64_t asInteger = static_cast<uint64_t>(aIn);
      if (asInteger > outMax) {
        return false;
      }
    }
  }

  // Checks if the integer is representable exactly as a floating point value of
  // a specific width.
  if constexpr (!inFloat && outFloat) {
    if constexpr (inSigned) {
      if (aIn < -safe_integer<Out>() || aIn > safe_integer<Out>()) {
        return false;
      }
    } else {
      if (aIn >= safe_integer_unsigned<Out>()) {
        return false;
      }
    }
  }

  if constexpr (noneFloat) {
    if constexpr (bothUnsigned) {
      if (aIn > select_widest(outMax)) {
        return false;
      }
    }
    if constexpr (bothSigned) {
      if (aIn > select_widest(outMax) || aIn < select_widest(outMin)) {
        return false;
      }
    }
    if constexpr (inSigned && !outSigned) {
      if (aIn < 0 || std::make_unsigned_t<In>(aIn) > outMax) {
        return false;
      }
    }
    if constexpr (!inSigned && outSigned) {
      if (aIn > select_widest(outMax)) {
        return false;
      }
    }
  }
  return true;
}
#pragma GCC diagnostic pop

}  // namespace detail

/**
 * Cast a value of type |From| to a value of type |To|, asserting that the cast
 * will be a safe cast per C++ (that is, that |to| is in the range of values
 * permitted for the type |From|).
 * In particular, this will fail if a integer cannot be represented exactly as a
 * floating point value, because it's too large.
 */
template <typename To, typename From>
inline To AssertedCast(const From aFrom) {
  static_assert(std::is_arithmetic_v<To> && std::is_arithmetic_v<From>);
  MOZ_ASSERT((detail::IsInBounds<From, To>(aFrom)));
  return static_cast<To>(aFrom);
}

/**
 * Cast a value of numeric type |From| to a value of numeric type |To|, release
 * asserting that the cast will be a safe cast per C++ (that is, that |to| is in
 * the range of values permitted for the type |From|).
 * In particular, this will fail if a integer cannot be represented exactly as a
 * floating point value, because it's too large.
 */
template <typename To, typename From>
inline To ReleaseAssertedCast(const From aFrom) {
  static_assert(std::is_arithmetic_v<To> && std::is_arithmetic_v<From>);
  MOZ_RELEASE_ASSERT((detail::IsInBounds<From, To>(aFrom)));
  return static_cast<To>(aFrom);
}

namespace detail {

template <typename From>
class LazyAssertedCastT final {
  const From mVal;

 public:
  explicit LazyAssertedCastT(const From val) : mVal(val) {}

  template <typename To>
  operator To() const {
    return AssertedCast<To>(mVal);
  }
};

}  // namespace detail

/**
 * Like AssertedCast, but infers |To| for AssertedCast lazily based on usage.
 * > uint8_t foo = LazyAssertedCast(1000);  // boom
 */
template <typename From>
inline auto LazyAssertedCast(const From val) {
  return detail::LazyAssertedCastT<From>(val);
}

}  // namespace mozilla

#endif /* mozilla_Casting_h */
