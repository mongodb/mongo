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

#include "NamespaceImports.h"

#include "frontend/ParserAtom.h"  // ParserAtomsTable, TaggedParserAtomIndex
#include "js/Conversions.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*

#include "vm/StringType.h"

namespace js {

class GlobalObject;
class StringBuffer;

[[nodiscard]] extern bool InitRuntimeNumberState(JSRuntime* rt);

// This is a no-op if built with JS_HAS_INTL_API.
extern void FinishRuntimeNumberState(JSRuntime* rt);

/* Initialize the Number class, returning its prototype object. */
extern JSObject* InitNumberClass(JSContext* cx, Handle<GlobalObject*> global);

/*
 * When base == 10, this function implements ToString() as specified by
 * ECMA-262-5 section 9.8.1; but note that it handles integers specially for
 * performance.  See also js::NumberToCString().
 */
template <AllowGC allowGC>
extern JSString* NumberToString(JSContext* cx, double d);

extern JSString* NumberToStringPure(JSContext* cx, double d);

extern JSAtom* NumberToAtom(JSContext* cx, double d);

frontend::TaggedParserAtomIndex NumberToParserAtom(
    JSContext* cx, frontend::ParserAtomsTable& parserAtoms, double d);

template <AllowGC allowGC>
extern JSLinearString* Int32ToString(JSContext* cx, int32_t i);

extern JSLinearString* Int32ToStringPure(JSContext* cx, int32_t i);

extern JSAtom* Int32ToAtom(JSContext* cx, int32_t si);

frontend::TaggedParserAtomIndex Int32ToParserAtom(
    JSContext* cx, frontend::ParserAtomsTable& parserAtoms, int32_t si);

// ES6 15.7.3.12
extern bool IsInteger(const Value& val);

extern bool IsInteger(double d);

/*
 * Convert an integer or double (contained in the given value) to a string and
 * append to the given buffer.
 */
[[nodiscard]] extern bool NumberValueToStringBuffer(JSContext* cx,
                                                    const Value& v,
                                                    StringBuffer& sb);

extern JSLinearString* IndexToString(JSContext* cx, uint32_t index);

/*
 * Usually a small amount of static storage is enough, but sometimes we need
 * to dynamically allocate much more.  This struct encapsulates that.
 * Dynamically allocated memory will be freed when the object is destroyed.
 */
struct ToCStringBuf {
  /*
   * The longest possible result that would need to fit in sbuf is
   * (-0x80000000).toString(2), which has length 33.  Longer cases are
   * possible, but they'll go in dbuf.
   */
  static const size_t sbufSize = 34;
  char sbuf[sbufSize];
  char* dbuf;

  ToCStringBuf();
  ~ToCStringBuf();
};

/*
 * Convert a number to a C string.  When base==10, this function implements
 * ToString() as specified by ECMA-262-5 section 9.8.1.  It handles integral
 * values cheaply.  Return nullptr if we ran out of memory.  See also
 * NumberToCString().
 */
extern char* NumberToCString(JSContext* cx, ToCStringBuf* cbuf, double d,
                             int base = 10);

/*
 * The largest positive integer such that all positive integers less than it
 * may be precisely represented using the IEEE-754 double-precision format.
 */
constexpr double DOUBLE_INTEGRAL_PRECISION_LIMIT = uint64_t(1) << 53;

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
    JSContext* cx, const CharT* start, const CharT* end, int base,
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
    JSContext* cx, const CharT* start, const CharT* end, int base,
    IntegerSeparatorHandling separatorHandling, double* dp) {
  decltype(ToRawChars(start)) realEnd;
  if (GetPrefixInteger(cx, ToRawChars(start), ToRawChars(end), base,
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
[[nodiscard]] extern bool GetDecimalInteger(JSContext* cx, const CharT* start,
                                            const CharT* end, double* dp);

/*
 * This is like GetDecimalInteger, but also allows non-integer numbers. It
 * should only be used when the characters are known to match |DecimalLiteral|,
 * cf. ES2020, 11.8.3 Numeric Literals.
 */
template <typename CharT>
[[nodiscard]] extern bool GetDecimalNonInteger(JSContext* cx,
                                               const CharT* start,
                                               const CharT* end, double* dp);

template <typename CharT>
bool CharsToNumber(JSContext* cx, const CharT* chars, size_t length,
                   double* result);

[[nodiscard]] extern bool StringToNumber(JSContext* cx, JSString* str,
                                         double* result);

[[nodiscard]] extern bool StringToNumberPure(JSContext* cx, JSString* str,
                                             double* result);

/*
 * Return true and set |*result| to the parsed number value if |str| can be
 * parsed as a number using the same rules as in |StringToNumber|. Otherwise
 * return false and leave |*result| in an indeterminate state.
 */
[[nodiscard]] extern bool MaybeStringToNumber(JSLinearString* str,
                                              double* result);

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
 * return the closest double number to the given input in dp.
 *
 * Also allows inputs of the form [+|-]Infinity, which produce an infinity of
 * the appropriate sign.  The case of the "Infinity" string must match exactly.
 * If the string does not contain a number, set *dEnd to begin and return 0.0
 * in *d.
 *
 * Return false if out of memory.
 */
template <typename CharT>
[[nodiscard]] extern bool js_strtod(JSContext* cx, const CharT* begin,
                                    const CharT* end, const CharT** dEnd,
                                    double* d);

namespace js {

/**
 * Like js_strtod, but for when the number always constitutes the entire range
 * (and so |dEnd| would be a value already known).
 */
template <typename CharT>
[[nodiscard]] extern bool FullStringToDouble(JSContext* cx, const CharT* begin,
                                             const CharT* end, double* d) {
  decltype(ToRawChars(begin)) realEnd;
  if (js_strtod(cx, ToRawChars(begin), ToRawChars(end), &realEnd, d)) {
    MOZ_ASSERT(end == static_cast<const void*>(realEnd));
    return true;
  }
  return false;
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
