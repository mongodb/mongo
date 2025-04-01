/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsnum_h
#define jsnum_h

#include "mozilla/FloatingPoint.h"
#include "mozilla/Range.h"
#include "mozilla/Utf8.h"

#include <limits>

#include "NamespaceImports.h"

#include "js/Conversions.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*

#include "vm/StringType.h"

namespace js {

namespace frontend {
class ParserAtomsTable;
class TaggedParserAtomIndex;
}  // namespace frontend

class GlobalObject;
class StringBuffer;

[[nodiscard]] extern bool InitRuntimeNumberState(JSRuntime* rt);

// This is a no-op if built with JS_HAS_INTL_API.
extern void FinishRuntimeNumberState(JSRuntime* rt);

/*
 * This function implements ToString() as specified by ECMA-262-5 section 9.8.1;
 * but note that it handles integers specially for performance.
 * See also js::NumberToCString().
 */
template <AllowGC allowGC>
extern JSString* NumberToString(JSContext* cx, double d);

extern JSString* NumberToStringPure(JSContext* cx, double d);

extern JSAtom* NumberToAtom(JSContext* cx, double d);

frontend::TaggedParserAtomIndex NumberToParserAtom(
    FrontendContext* fc, frontend::ParserAtomsTable& parserAtoms, double d);

template <AllowGC allowGC>
extern JSLinearString* Int32ToString(JSContext* cx, int32_t i);

template <AllowGC allowGC>
extern JSLinearString* Int32ToStringWithHeap(JSContext* cx, int32_t i,
                                             gc::Heap heap);

extern JSLinearString* Int32ToStringPure(JSContext* cx, int32_t i);

extern JSString* Int32ToStringWithBase(JSContext* cx, int32_t i, int32_t base,
                                       bool lowerCase);

extern JSAtom* Int32ToAtom(JSContext* cx, int32_t si);

frontend::TaggedParserAtomIndex Int32ToParserAtom(
    FrontendContext* fc, frontend::ParserAtomsTable& parserAtoms, int32_t si);

// ES6 15.7.3.12
extern bool IsInteger(double d);

/*
 * Convert an integer or double (contained in the given value) to a string and
 * append to the given buffer.
 */
[[nodiscard]] extern bool NumberValueToStringBuffer(const Value& v,
                                                    StringBuffer& sb);

extern JSLinearString* IndexToString(JSContext* cx, uint32_t index);

struct ToCStringBuf {
  char sbuf[JS::MaximumNumberToStringLength] = {};
};

struct Int32ToCStringBuf {
  // The amount of space large enough to store the null-terminated result of
  // |ToString| on any int32.
  //
  // We use the same amount for uint32 (base 10 and base 16), even though uint32
  // only need 11 characters (base 10) resp. 9 characters (base 16) instead of
  // 12 characters for int32 in base 10.
  static constexpr size_t MaximumInt32ToStringLength =
      std::numeric_limits<int32_t>::digits10 +
      1 +  // account for the largest possible int32 value
      1 +  // sign for negative numbers
      1    // null character
      ;

  char sbuf[MaximumInt32ToStringLength] = {};
};

// Convert a number to a C string.  This function implements ToString() as
// specified by ECMA-262-5 section 9.8.1.  It handles integral values cheaply.
// Infallible: always returns a non-nullptr string.
// The optional `length` out-param is set to the string length of the result.
extern char* NumberToCString(ToCStringBuf* cbuf, double d,
                             size_t* length = nullptr);

extern char* Int32ToCString(Int32ToCStringBuf* cbuf, int32_t value,
                            size_t* length = nullptr);

extern char* Uint32ToCString(Int32ToCStringBuf* cbuf, uint32_t value,
                             size_t* length = nullptr);

// Like NumberToCString, but accepts only unsigned integers and uses base 16.
// Infallible: always returns a non-nullptr string.
// The optional `length` out-param is set to the string length of the result.
extern char* Uint32ToHexCString(Int32ToCStringBuf* cbuf, uint32_t value,
                                size_t* length = nullptr);

/*
 * The largest positive integer such that all positive integers less than it
 * may be precisely represented using the IEEE-754 double-precision format.
 */
constexpr double DOUBLE_INTEGRAL_PRECISION_LIMIT = uint64_t(1) << 53;

/*
 * The smallest positive double such that all positive doubles larger or equal
 * than it have an exact decimal representation without exponential form.
 */
constexpr double DOUBLE_DECIMAL_IN_SHORTEST_LOW = 1.0e-6;

/*
 * The largest positive double such that all positive doubles less than it
 * have an exact decimal representation without exponential form.
 */
constexpr double DOUBLE_DECIMAL_IN_SHORTEST_HIGH = 1.0e21;

/*
 * Parse a decimal number encoded in |chars|.  The decimal number must be
 * sufficiently small that it will not overflow the integrally-precise range of
 * the double type -- that is, the number will be smaller than
 * DOUBLE_INTEGRAL_PRECISION_LIMIT
 */
template <typename CharT>
extern double ParseDecimalNumber(const mozilla::Range<const CharT> chars);

enum class IntegerSeparatorHandling : bool { None, SkipUnderscore };

/*
 * Compute the positive integer of the given base described immediately at the
 * start of the range [start, end) -- no whitespace-skipping, no magical
 * leading-"0" octal or leading-"0x" hex behavior, no "+"/"-" parsing, just
 * reading the digits of the integer.  Return the index one past the end of the
 * digits of the integer in *endp, and return the integer itself in *dp.  If
 * base is 10 or a power of two the returned integer is the closest possible
 * double; otherwise extremely large integers may be slightly inaccurate.
 *
 * The |separatorHandling| controls whether or not numeric separators can be
 * part of integer string. If the option is enabled, all '_' characters in the
 * string are ignored. Underscore characters must not appear directly next to
 * each other, e.g. '1__2' will lead to an assertion.
 *
 * If [start, end) does not begin with a number with the specified base,
 * *dp == 0 and *endp == start upon return.
 */
template <typename CharT>
[[nodiscard]] extern bool GetPrefixInteger(
    const CharT* start, const CharT* end, int base,
    IntegerSeparatorHandling separatorHandling, const CharT** endp, double* dp);

inline const char16_t* ToRawChars(const char16_t* units) { return units; }

inline const unsigned char* ToRawChars(const unsigned char* units) {
  return units;
}

inline const unsigned char* ToRawChars(const mozilla::Utf8Unit* units) {
  return mozilla::Utf8AsUnsignedChars(units);
}

/**
 * Like GetPrefixInteger, but [start, end) must all be digits in the given
 * base (and so this function doesn't take a useless outparam).
 */
template <typename CharT>
[[nodiscard]] extern bool GetFullInteger(
    const CharT* start, const CharT* end, int base,
    IntegerSeparatorHandling separatorHandling, double* dp) {
  decltype(ToRawChars(start)) realEnd;
  if (GetPrefixInteger(ToRawChars(start), ToRawChars(end), base,
                       separatorHandling, &realEnd, dp)) {
    MOZ_ASSERT(end == static_cast<const void*>(realEnd));
    return true;
  }
  return false;
}

/*
 * This is like GetPrefixInteger, but only deals with base 10, always ignores
 * '_', and doesn't have an |endp| outparam. It should only be used when the
 * characters are known to match |DecimalIntegerLiteral|, cf. ES2020, 11.8.3
 * Numeric Literals.
 */
template <typename CharT>
[[nodiscard]] extern bool GetDecimalInteger(const CharT* start,
                                            const CharT* end, double* dp);

/*
 * This is like GetDecimalInteger, but also allows non-integer numbers. It
 * should only be used when the characters are known to match |DecimalLiteral|,
 * cf. ES2020, 11.8.3 Numeric Literals.
 */
template <typename CharT>
[[nodiscard]] extern bool GetDecimal(const CharT* start, const CharT* end,
                                     double* dp);

template <typename CharT>
double CharsToNumber(const CharT* chars, size_t length);

[[nodiscard]] extern bool StringToNumber(JSContext* cx, JSString* str,
                                         double* result);

[[nodiscard]] extern bool StringToNumberPure(JSContext* cx, JSString* str,
                                             double* result);

// Infallible version of StringToNumber for linear strings.
extern double LinearStringToNumber(JSLinearString* str);

// Parse the input string as if Number.parseInt had been called.
extern bool NumberParseInt(JSContext* cx, JS::HandleString str, int32_t radix,
                           JS::MutableHandleValue result);

/* ES5 9.3 ToNumber, overwriting *vp with the appropriate number value. */
[[nodiscard]] MOZ_ALWAYS_INLINE bool ToNumber(JSContext* cx,
                                              JS::MutableHandleValue vp) {
  if (vp.isNumber()) {
    return true;
  }
  double d;
  extern JS_PUBLIC_API bool ToNumberSlow(JSContext * cx, HandleValue v,
                                         double* dp);
  if (!ToNumberSlow(cx, vp, &d)) {
    return false;
  }

  vp.setNumber(d);
  return true;
}

bool ToNumericSlow(JSContext* cx, JS::MutableHandleValue vp);

// BigInt proposal section 3.1.6
[[nodiscard]] MOZ_ALWAYS_INLINE bool ToNumeric(JSContext* cx,
                                               JS::MutableHandleValue vp) {
  if (vp.isNumeric()) {
    return true;
  }
  return ToNumericSlow(cx, vp);
}

bool ToInt32OrBigIntSlow(JSContext* cx, JS::MutableHandleValue vp);

[[nodiscard]] MOZ_ALWAYS_INLINE bool ToInt32OrBigInt(
    JSContext* cx, JS::MutableHandleValue vp) {
  if (vp.isInt32()) {
    return true;
  }
  return ToInt32OrBigIntSlow(cx, vp);
}

} /* namespace js */

/*
 * Similar to strtod except that it replaces overflows with infinities of the
 * correct sign, and underflows with zeros of the correct sign.  Guaranteed to
 * return the closest double number to the given input.
 *
 * Also allows inputs of the form [+|-]Infinity, which produce an infinity of
 * the appropriate sign.  The case of the "Infinity" string must match exactly.
 * If the string does not contain a number, set *dEnd to begin and return 0.0.
 */
template <typename CharT>
[[nodiscard]] extern double js_strtod(const CharT* begin, const CharT* end,
                                      const CharT** dEnd);

namespace js {

/**
 * Like js_strtod, but for when the number always constitutes the entire range
 * (and so |dEnd| would be a value already known).
 */
template <typename CharT>
[[nodiscard]] extern double FullStringToDouble(const CharT* begin,
                                               const CharT* end) {
  decltype(ToRawChars(begin)) realEnd;
  double d = js_strtod(ToRawChars(begin), ToRawChars(end), &realEnd);
  MOZ_ASSERT(end == static_cast<const void*>(realEnd));
  return d;
}

[[nodiscard]] extern bool ThisNumberValueForToLocaleString(JSContext* cx,
                                                           unsigned argc,
                                                           Value* vp);

[[nodiscard]] extern bool num_valueOf(JSContext* cx, unsigned argc, Value* vp);

/*
 * Returns true if the given value is definitely an index: that is, the value
 * is a number that's an unsigned 32-bit integer.
 *
 * This method prioritizes common-case speed over accuracy in every case.  It
 * can produce false negatives (but not false positives): some values which are
 * indexes will be reported not to be indexes by this method.  Users must
 * consider this possibility when using this method.
 */
static MOZ_ALWAYS_INLINE bool IsDefinitelyIndex(const Value& v,
                                                uint32_t* indexp) {
  if (v.isInt32() && v.toInt32() >= 0) {
    *indexp = v.toInt32();
    return true;
  }

  int32_t i;
  if (v.isDouble() && mozilla::NumberEqualsInt32(v.toDouble(), &i) && i >= 0) {
    *indexp = uint32_t(i);
    return true;
  }

  if (v.isString() && v.toString()->hasIndexValue()) {
    *indexp = v.toString()->getIndexValue();
    return true;
  }

  return false;
}

// ES2020 draft rev 6b05bc56ba4e3c7a2b9922c4282d9eb844426d9b
// 7.1.5 ToInteger ( argument )
[[nodiscard]] static inline bool ToInteger(JSContext* cx, HandleValue v,
                                           double* dp) {
  if (v.isInt32()) {
    *dp = v.toInt32();
    return true;
  }
  if (v.isDouble()) {
    *dp = v.toDouble();
  } else if (v.isString() && v.toString()->hasIndexValue()) {
    *dp = v.toString()->getIndexValue();
    return true;
  } else {
    extern JS_PUBLIC_API bool ToNumberSlow(JSContext * cx, HandleValue v,
                                           double* dp);
    if (!ToNumberSlow(cx, v, dp)) {
      return false;
    }
  }
  *dp = JS::ToInteger(*dp);
  return true;
}

/* ES2017 draft 7.1.17 ToIndex
 *
 * Return true and set |*index| to the integer value if |v| is a valid
 * integer index value. Otherwise report a RangeError and return false.
 *
 * The returned index will always be in the range 0 <= *index <= 2^53-1.
 */
[[nodiscard]] extern bool ToIndexSlow(JSContext* cx, JS::HandleValue v,
                                      const unsigned errorNumber,
                                      uint64_t* index);

[[nodiscard]] static inline bool ToIndex(JSContext* cx, JS::HandleValue v,
                                         const unsigned errorNumber,
                                         uint64_t* index) {
  if (v.isInt32()) {
    int32_t i = v.toInt32();
    if (i >= 0) {
      *index = uint64_t(i);
      return true;
    }
  }
  return ToIndexSlow(cx, v, errorNumber, index);
}

[[nodiscard]] static inline bool ToIndex(JSContext* cx, JS::HandleValue v,
                                         uint64_t* index) {
  return ToIndex(cx, v, JSMSG_BAD_INDEX, index);
}

} /* namespace js */

#endif /* jsnum_h */
