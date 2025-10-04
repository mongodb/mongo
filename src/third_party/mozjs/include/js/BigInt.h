/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* BigInt. */

#ifndef js_BigInt_h
#define js_BigInt_h

#include "mozilla/Span.h"  // mozilla::Span

#include <limits>       // std::numeric_limits
#include <stdint.h>     // int64_t, uint64_t
#include <type_traits>  // std::enable_if_t, std::{true,false}_type, std::is_{integral,signed,unsigned}_v

#include "jstypes.h"  // JS_PUBLIC_API
#include "js/TypeDecls.h"

namespace mozilla {
template <typename T>
class Range;
}

namespace JS {

class JS_PUBLIC_API BigInt;

namespace detail {

using Int64Limits = std::numeric_limits<int64_t>;
using Uint64Limits = std::numeric_limits<uint64_t>;

extern JS_PUBLIC_API BigInt* BigIntFromInt64(JSContext* cx, int64_t num);
extern JS_PUBLIC_API BigInt* BigIntFromUint64(JSContext* cx, uint64_t num);
extern JS_PUBLIC_API BigInt* BigIntFromBool(JSContext* cx, bool b);

template <typename T, typename = void>
struct NumberToBigIntConverter;

template <typename SignedIntT>
struct NumberToBigIntConverter<
    SignedIntT,
    std::enable_if_t<
        std::is_integral_v<SignedIntT> && std::is_signed_v<SignedIntT> &&
        Int64Limits::min() <= std::numeric_limits<SignedIntT>::min() &&
        std::numeric_limits<SignedIntT>::max() <= Int64Limits::max()>> {
  static BigInt* convert(JSContext* cx, SignedIntT num) {
    return BigIntFromInt64(cx, num);
  }
};

template <typename UnsignedIntT>
struct NumberToBigIntConverter<
    UnsignedIntT,
    std::enable_if_t<
        std::is_integral_v<UnsignedIntT> && std::is_unsigned_v<UnsignedIntT> &&
        std::numeric_limits<UnsignedIntT>::max() <= Uint64Limits::max()>> {
  static BigInt* convert(JSContext* cx, UnsignedIntT num) {
    return BigIntFromUint64(cx, num);
  }
};

template <>
struct NumberToBigIntConverter<bool> {
  static BigInt* convert(JSContext* cx, bool b) {
    return BigIntFromBool(cx, b);
  }
};

extern JS_PUBLIC_API bool BigIntIsInt64(const BigInt* bi, int64_t* result);
extern JS_PUBLIC_API bool BigIntIsUint64(const BigInt* bi, uint64_t* result);

template <typename T, typename = void>
struct BigIntToNumberChecker;

template <typename SignedIntT>
struct BigIntToNumberChecker<
    SignedIntT,
    std::enable_if_t<
        std::is_integral_v<SignedIntT> && std::is_signed_v<SignedIntT> &&
        Int64Limits::min() <= std::numeric_limits<SignedIntT>::min() &&
        std::numeric_limits<SignedIntT>::max() <= Int64Limits::max()>> {
  using TypeLimits = std::numeric_limits<SignedIntT>;

  static bool fits(const BigInt* bi, SignedIntT* result) {
    int64_t innerResult;
    if (!BigIntIsInt64(bi, &innerResult)) {
      return false;
    }
    if (TypeLimits::min() <= innerResult && innerResult <= TypeLimits::max()) {
      *result = SignedIntT(innerResult);
      return true;
    }
    return false;
  }
};

template <typename UnsignedIntT>
struct BigIntToNumberChecker<
    UnsignedIntT,
    std::enable_if_t<
        std::is_integral_v<UnsignedIntT> && std::is_unsigned_v<UnsignedIntT> &&
        std::numeric_limits<UnsignedIntT>::max() <= Uint64Limits::max()>> {
  static bool fits(const BigInt* bi, UnsignedIntT* result) {
    uint64_t innerResult;
    if (!BigIntIsUint64(bi, &innerResult)) {
      return false;
    }
    if (innerResult <= std::numeric_limits<UnsignedIntT>::max()) {
      *result = UnsignedIntT(innerResult);
      return true;
    }
    return false;
  }
};

}  // namespace detail

/**
 * Create a BigInt from an integer value. All integral types not larger than 64
 * bits in size are supported.
 */
template <typename NumericT>
static inline BigInt* NumberToBigInt(JSContext* cx, NumericT val) {
  return detail::NumberToBigIntConverter<NumericT>::convert(cx, val);
}

/**
 * Create a BigInt from a floating-point value. If the number isn't integral
 * (that is, if it's NaN, an infinity, or contains a fractional component),
 * this function returns null and throws an exception.
 *
 * Passing -0.0 will produce the bigint 0n.
 */
extern JS_PUBLIC_API BigInt* NumberToBigInt(JSContext* cx, double num);

/**
 * Create a BigInt by parsing a string using the ECMAScript StringToBigInt
 * algorithm (https://tc39.es/ecma262/#sec-stringtobigint). Latin1 and two-byte
 * character ranges are supported. It may be convenient to use
 * JS::ConstLatin1Chars or JS::ConstTwoByteChars.
 *
 * (StringToBigInt performs parsing similar to that performed by the |Number|
 * global function when passed a string, but it doesn't allow infinities,
 * decimal points, or exponential notation, and neither algorithm allows numeric
 * separators or an 'n' suffix character. This fast-and-loose description is
 * offered purely as a convenience to the reader: see the specification
 * algorithm for exact behavior.)
 *
 * If parsing fails, this function returns null and throws an exception.
 */
extern JS_PUBLIC_API BigInt* StringToBigInt(
    JSContext* cx, const mozilla::Range<const Latin1Char>& chars);

extern JS_PUBLIC_API BigInt* StringToBigInt(
    JSContext* cx, const mozilla::Range<const char16_t>& chars);

/**
 * Create a BigInt by parsing a string consisting of an optional sign character
 * followed by one or more alphanumeric ASCII digits in the provided radix.
 *
 * If the radix is not in the range [2, 36], or the string fails to parse, this
 * function returns null and throws an exception.
 */
extern JS_PUBLIC_API BigInt* SimpleStringToBigInt(
    JSContext* cx, mozilla::Span<const char> chars, uint8_t radix);

/**
 * Convert a JS::Value to a BigInt using the ECMAScript ToBigInt algorithm
 * (https://tc39.es/ecma262/#sec-tobigint).
 *
 * (Note in particular that this will throw if passed a value whose type is
 * 'number'. To convert a number to a BigInt, use one of the overloads of
 * JS::NumberToBigInt().)
 */
extern JS_PUBLIC_API BigInt* ToBigInt(JSContext* cx, Handle<Value> val);

/**
 * Convert the given BigInt, modulo 2**64, to a signed 64-bit integer.
 */
extern JS_PUBLIC_API int64_t ToBigInt64(const BigInt* bi);

/**
 * Convert the given BigInt, modulo 2**64, to an unsigned 64-bit integer.
 */
extern JS_PUBLIC_API uint64_t ToBigUint64(const BigInt* bi);

/**
 * Convert the given BigInt to a Number value as if calling the Number
 * constructor on it
 * (https://tc39.es/ecma262/#sec-number-constructor-number-value). The value
 * may be rounded if it doesn't fit without loss of precision.
 */
extern JS_PUBLIC_API double BigIntToNumber(const BigInt* bi);

/**
 * Return true if the given BigInt is negative.
 */
extern JS_PUBLIC_API bool BigIntIsNegative(const BigInt* bi);

/**
 * Return true if the given BigInt fits inside the given NumericT type without
 * loss of precision, and store the value in the out parameter. Otherwise return
 * false and leave the value of the out parameter unspecified.
 */
template <typename NumericT>
static inline bool BigIntFits(const BigInt* bi, NumericT* out) {
  return detail::BigIntToNumberChecker<NumericT>::fits(bi, out);
}

/**
 * Same as BigIntFits(), but checks if the value fits inside a JS Number value.
 */
extern JS_PUBLIC_API bool BigIntFitsNumber(const BigInt* bi, double* out);

/**
 * Convert the given BigInt to a String value as if toString() were called on
 * it.
 *
 * If the radix is not in the range [2, 36], then this function returns null and
 * throws an exception.
 */
extern JS_PUBLIC_API JSString* BigIntToString(JSContext* cx, Handle<BigInt*> bi,
                                              uint8_t radix);

}  // namespace JS

#endif /* js_BigInt_h */
