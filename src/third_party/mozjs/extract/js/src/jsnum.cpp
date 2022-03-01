/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS number type and wrapper class.
 */

#include "jsnum.h"

#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"
#include "mozilla/RangedPtr.h"
#include "mozilla/TextUtils.h"
#include "mozilla/Utf8.h"

#include <iterator>
#ifdef HAVE_LOCALECONV
#  include <locale.h>
#endif
#include <math.h>
#include <string.h>  // memmove

#include "jstypes.h"

#include "double-conversion/double-conversion.h"
#include "frontend/ParserAtom.h"  // frontend::{ParserAtomsTable, TaggedParserAtomIndex}
#include "jit/InlinableNatives.h"
#include "js/CharacterEncoding.h"
#include "js/Conversions.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#if !JS_HAS_INTL_API
#  include "js/LocaleSensitive.h"
#endif
#include "js/PropertySpec.h"
#include "util/DoubleToString.h"
#include "util/Memory.h"
#include "util/StringBuffer.h"
#include "vm/BigIntType.h"
#include "vm/GlobalObject.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/WellKnownAtom.h"  // js_*_str

#include "vm/Compartment-inl.h"  // For js::UnwrapAndTypeCheckThis
#include "vm/NativeObject-inl.h"
#include "vm/NumberObject-inl.h"
#include "vm/StringType-inl.h"

using namespace js;

using mozilla::Abs;
using mozilla::AsciiAlphanumericToNumber;
using mozilla::IsAsciiAlphanumeric;
using mozilla::IsAsciiDigit;
using mozilla::Maybe;
using mozilla::MinNumberValue;
using mozilla::NegativeInfinity;
using mozilla::NumberEqualsInt32;
using mozilla::PositiveInfinity;
using mozilla::RangedPtr;
using mozilla::Utf8AsUnsignedChars;
using mozilla::Utf8Unit;

using JS::AutoCheckCannotGC;
using JS::GenericNaN;
using JS::ToInt16;
using JS::ToInt32;
using JS::ToInt64;
using JS::ToInt8;
using JS::ToUint16;
using JS::ToUint32;
using JS::ToUint64;
using JS::ToUint8;

static bool EnsureDtoaState(JSContext* cx) {
  if (!cx->dtoaState) {
    cx->dtoaState = NewDtoaState();
    if (!cx->dtoaState) {
      return false;
    }
  }
  return true;
}

template <typename CharT>
static inline void AssertWellPlacedNumericSeparator(const CharT* s,
                                                    const CharT* start,
                                                    const CharT* end) {
  MOZ_ASSERT(start < end, "string is non-empty");
  MOZ_ASSERT(s > start, "number can't start with a separator");
  MOZ_ASSERT(s + 1 < end,
             "final character in a numeric literal can't be a separator");
  MOZ_ASSERT(*(s + 1) != '_',
             "separator can't be followed by another separator");
  MOZ_ASSERT(*(s - 1) != '_',
             "separator can't be preceded by another separator");
}

/*
 * If we're accumulating a decimal number and the number is >= 2^53, then the
 * fast result from the loop in Get{Prefix,Decimal}Integer may be inaccurate.
 * Call js_strtod_harder to get the correct answer.
 */
template <typename CharT>
static bool ComputeAccurateDecimalInteger(JSContext* cx, const CharT* start,
                                          const CharT* end, double* dp) {
  size_t length = end - start;
  auto cstr = cx->make_pod_array<char>(length + 1);
  if (!cstr) {
    return false;
  }

  size_t j = 0;
  for (size_t i = 0; i < length; i++) {
    char c = char(start[i]);
    if (c == '_') {
      AssertWellPlacedNumericSeparator(start + i, start, end);
      continue;
    }
    MOZ_ASSERT(IsAsciiAlphanumeric(c));
    cstr[j++] = c;
  }
  cstr[j] = 0;

  if (!EnsureDtoaState(cx)) {
    return false;
  }

  char* estr;
  *dp = js_strtod_harder(cx->dtoaState, cstr.get(), &estr);

  return true;
}

namespace {

template <typename CharT>
class BinaryDigitReader {
  const int base;     /* Base of number; must be a power of 2 */
  int digit;          /* Current digit value in radix given by base */
  int digitMask;      /* Mask to extract the next bit from digit */
  const CharT* cur;   /* Pointer to the remaining digits */
  const CharT* start; /* Pointer to the start of the string */
  const CharT* end;   /* Pointer to first non-digit */

 public:
  BinaryDigitReader(int base, const CharT* start, const CharT* end)
      : base(base),
        digit(0),
        digitMask(0),
        cur(start),
        start(start),
        end(end) {}

  /* Return the next binary digit from the number, or -1 if done. */
  int nextDigit() {
    if (digitMask == 0) {
      if (cur == end) {
        return -1;
      }

      int c = *cur++;
      if (c == '_') {
        AssertWellPlacedNumericSeparator(cur - 1, start, end);
        c = *cur++;
      }

      MOZ_ASSERT(IsAsciiAlphanumeric(c));
      digit = AsciiAlphanumericToNumber(c);
      digitMask = base >> 1;
    }

    int bit = (digit & digitMask) != 0;
    digitMask >>= 1;
    return bit;
  }
};

} /* anonymous namespace */

/*
 * The fast result might also have been inaccurate for power-of-two bases. This
 * happens if the addition in value * 2 + digit causes a round-down to an even
 * least significant mantissa bit when the first dropped bit is a one.  If any
 * of the following digits in the number (which haven't been added in yet) are
 * nonzero, then the correct action would have been to round up instead of
 * down.  An example occurs when reading the number 0x1000000000000081, which
 * rounds to 0x1000000000000000 instead of 0x1000000000000100.
 */
template <typename CharT>
static double ComputeAccurateBinaryBaseInteger(const CharT* start,
                                               const CharT* end, int base) {
  BinaryDigitReader<CharT> bdr(base, start, end);

  /* Skip leading zeroes. */
  int bit;
  do {
    bit = bdr.nextDigit();
  } while (bit == 0);

  MOZ_ASSERT(bit == 1);  // guaranteed by Get{Prefix,Decimal}Integer

  /* Gather the 53 significant bits (including the leading 1). */
  double value = 1.0;
  for (int j = 52; j > 0; j--) {
    bit = bdr.nextDigit();
    if (bit < 0) {
      return value;
    }
    value = value * 2 + bit;
  }

  /* bit2 is the 54th bit (the first dropped from the mantissa). */
  int bit2 = bdr.nextDigit();
  if (bit2 >= 0) {
    double factor = 2.0;
    int sticky = 0; /* sticky is 1 if any bit beyond the 54th is 1 */
    int bit3;

    while ((bit3 = bdr.nextDigit()) >= 0) {
      sticky |= bit3;
      factor *= 2;
    }
    value += bit2 & (bit | sticky);
    value *= factor;
  }

  return value;
}

template <typename CharT>
double js::ParseDecimalNumber(const mozilla::Range<const CharT> chars) {
  MOZ_ASSERT(chars.length() > 0);
  uint64_t dec = 0;
  RangedPtr<const CharT> s = chars.begin(), end = chars.end();
  do {
    CharT c = *s;
    MOZ_ASSERT('0' <= c && c <= '9');
    uint8_t digit = c - '0';
    uint64_t next = dec * 10 + digit;
    MOZ_ASSERT(next < DOUBLE_INTEGRAL_PRECISION_LIMIT,
               "next value won't be an integrally-precise double");
    dec = next;
  } while (++s < end);
  return static_cast<double>(dec);
}

template double js::ParseDecimalNumber(
    const mozilla::Range<const Latin1Char> chars);

template double js::ParseDecimalNumber(
    const mozilla::Range<const char16_t> chars);

template <typename CharT>
static bool GetPrefixInteger(const CharT* start, const CharT* end, int base,
                             IntegerSeparatorHandling separatorHandling,
                             const CharT** endp, double* dp) {
  MOZ_ASSERT(start <= end);
  MOZ_ASSERT(2 <= base && base <= 36);

  const CharT* s = start;
  double d = 0.0;
  for (; s < end; s++) {
    CharT c = *s;
    if (!IsAsciiAlphanumeric(c)) {
      if (c == '_' &&
          separatorHandling == IntegerSeparatorHandling::SkipUnderscore) {
        AssertWellPlacedNumericSeparator(s, start, end);
        continue;
      }
      break;
    }

    uint8_t digit = AsciiAlphanumericToNumber(c);
    if (digit >= base) {
      break;
    }

    d = d * base + digit;
  }

  *endp = s;
  *dp = d;

  /* If we haven't reached the limit of integer precision, we're done. */
  if (d < DOUBLE_INTEGRAL_PRECISION_LIMIT) {
    return true;
  }

  /*
   * Otherwise compute the correct integer from the prefix of valid digits
   * if we're computing for base ten or a power of two.  Don't worry about
   * other bases; see ES2018, 18.2.5 `parseInt(string, radix)`, step 13.
   */
  if (base == 10) {
    return false;
  }

  if ((base & (base - 1)) == 0) {
    *dp = ComputeAccurateBinaryBaseInteger(start, s, base);
  }

  return true;
}

template <typename CharT>
bool js::GetPrefixInteger(JSContext* cx, const CharT* start, const CharT* end,
                          int base, IntegerSeparatorHandling separatorHandling,
                          const CharT** endp, double* dp) {
  if (::GetPrefixInteger(start, end, base, separatorHandling, endp, dp)) {
    return true;
  }

  // Can only fail for base 10.
  MOZ_ASSERT(base == 10);

  return ComputeAccurateDecimalInteger(cx, start, *endp, dp);
}

namespace js {

template bool GetPrefixInteger(JSContext* cx, const char16_t* start,
                               const char16_t* end, int base,
                               IntegerSeparatorHandling separatorHandling,
                               const char16_t** endp, double* dp);

template bool GetPrefixInteger(JSContext* cx, const Latin1Char* start,
                               const Latin1Char* end, int base,
                               IntegerSeparatorHandling separatorHandling,
                               const Latin1Char** endp, double* dp);

}  // namespace js

template <typename CharT>
bool js::GetDecimalInteger(JSContext* cx, const CharT* start, const CharT* end,
                           double* dp) {
  MOZ_ASSERT(start <= end);

  const CharT* s = start;
  double d = 0.0;
  for (; s < end; s++) {
    CharT c = *s;
    if (c == '_') {
      AssertWellPlacedNumericSeparator(s, start, end);
      continue;
    }
    MOZ_ASSERT(IsAsciiDigit(c));
    int digit = c - '0';
    d = d * 10 + digit;
  }

  *dp = d;

  // If we haven't reached the limit of integer precision, we're done.
  if (d < DOUBLE_INTEGRAL_PRECISION_LIMIT) {
    return true;
  }

  // Otherwise compute the correct integer from the prefix of valid digits.
  return ComputeAccurateDecimalInteger(cx, start, s, dp);
}

namespace js {

template bool GetDecimalInteger(JSContext* cx, const char16_t* start,
                                const char16_t* end, double* dp);

template bool GetDecimalInteger(JSContext* cx, const Latin1Char* start,
                                const Latin1Char* end, double* dp);

template <>
bool GetDecimalInteger<Utf8Unit>(JSContext* cx, const Utf8Unit* start,
                                 const Utf8Unit* end, double* dp) {
  return GetDecimalInteger(cx, Utf8AsUnsignedChars(start),
                           Utf8AsUnsignedChars(end), dp);
}

}  // namespace js

template <typename CharT>
bool js::GetDecimalNonInteger(JSContext* cx, const CharT* start,
                              const CharT* end, double* dp) {
  MOZ_ASSERT(start <= end);

  size_t length = end - start;
  Vector<char, 32> chars(cx);
  if (!chars.growByUninitialized(length + 1)) {
    return false;
  }

  const CharT* s = start;
  size_t i = 0;
  for (; s < end; s++) {
    CharT c = *s;
    if (c == '_') {
      AssertWellPlacedNumericSeparator(s, start, end);
      continue;
    }
    MOZ_ASSERT(IsAsciiDigit(c) || c == '.' || c == 'e' || c == 'E' ||
               c == '+' || c == '-');
    chars[i++] = char(c);
  }
  chars[i] = 0;

  if (!EnsureDtoaState(cx)) {
    return false;
  }

  char* ep;
  *dp = js_strtod_harder(cx->dtoaState, chars.begin(), &ep);
  MOZ_ASSERT(ep >= chars.begin());

  return true;
}

namespace js {

template bool GetDecimalNonInteger(JSContext* cx, const char16_t* start,
                                   const char16_t* end, double* dp);

template bool GetDecimalNonInteger(JSContext* cx, const Latin1Char* start,
                                   const Latin1Char* end, double* dp);

template <>
bool GetDecimalNonInteger<Utf8Unit>(JSContext* cx, const Utf8Unit* start,
                                    const Utf8Unit* end, double* dp) {
  return GetDecimalNonInteger(cx, Utf8AsUnsignedChars(start),
                              Utf8AsUnsignedChars(end), dp);
}

}  // namespace js

static bool num_parseFloat(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() == 0) {
    args.rval().setNaN();
    return true;
  }

  if (args[0].isNumber()) {
    // ToString(-0) is "0", handle it accordingly.
    if (args[0].isDouble() && args[0].toDouble() == 0.0) {
      args.rval().setInt32(0);
    } else {
      args.rval().set(args[0]);
    }
    return true;
  }

  JSString* str = ToString<CanGC>(cx, args[0]);
  if (!str) {
    return false;
  }

  if (str->hasIndexValue()) {
    args.rval().setNumber(str->getIndexValue());
    return true;
  }

  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  double d;
  AutoCheckCannotGC nogc;
  if (linear->hasLatin1Chars()) {
    const Latin1Char* begin = linear->latin1Chars(nogc);
    const Latin1Char* end;
    if (!js_strtod(cx, begin, begin + linear->length(), &end, &d)) {
      return false;
    }
    if (end == begin) {
      d = GenericNaN();
    }
  } else {
    const char16_t* begin = linear->twoByteChars(nogc);
    const char16_t* end;
    if (!js_strtod(cx, begin, begin + linear->length(), &end, &d)) {
      return false;
    }
    if (end == begin) {
      d = GenericNaN();
    }
  }

  args.rval().setDouble(d);
  return true;
}

template <typename CharT>
static bool ParseIntImpl(JSContext* cx, const CharT* chars, size_t length,
                         bool stripPrefix, int32_t radix, double* res) {
  /* Step 2. */
  const CharT* end = chars + length;
  const CharT* s = SkipSpace(chars, end);

  MOZ_ASSERT(chars <= s);
  MOZ_ASSERT(s <= end);

  /* Steps 3-4. */
  bool negative = (s != end && s[0] == '-');

  /* Step 5. */
  if (s != end && (s[0] == '-' || s[0] == '+')) {
    s++;
  }

  /* Step 10. */
  if (stripPrefix) {
    if (end - s >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
      s += 2;
      radix = 16;
    }
  }

  /* Steps 11-15. */
  const CharT* actualEnd;
  double d;
  if (!GetPrefixInteger(cx, s, end, radix, IntegerSeparatorHandling::None,
                        &actualEnd, &d)) {
    return false;
  }

  if (s == actualEnd) {
    *res = GenericNaN();
  } else {
    *res = negative ? -d : d;
  }
  return true;
}

/* ES5 15.1.2.2. */
static bool num_parseInt(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  /* Fast paths and exceptional cases. */
  if (args.length() == 0) {
    args.rval().setNaN();
    return true;
  }

  if (args.length() == 1 || (args[1].isInt32() && (args[1].toInt32() == 0 ||
                                                   args[1].toInt32() == 10))) {
    if (args[0].isInt32()) {
      args.rval().set(args[0]);
      return true;
    }

    /*
     * Step 1 is |inputString = ToString(string)|. When string >=
     * 1e21, ToString(string) is in the form "NeM". 'e' marks the end of
     * the word, which would mean the result of parseInt(string) should be |N|.
     *
     * To preserve this behaviour, we can't use the fast-path when string >
     * 1e21, or else the result would be |NeM|.
     *
     * The same goes for values smaller than 1.0e-6, because the string would be
     * in the form of "Ne-M".
     */
    if (args[0].isDouble()) {
      double d = args[0].toDouble();
      if (1.0e-6 < d && d < 1.0e21) {
        args.rval().setNumber(floor(d));
        return true;
      }
      if (-1.0e21 < d && d < -1.0e-6) {
        args.rval().setNumber(-floor(-d));
        return true;
      }
      if (d == 0.0) {
        args.rval().setInt32(0);
        return true;
      }
    }

    if (args[0].isString()) {
      JSString* str = args[0].toString();
      if (str->hasIndexValue()) {
        args.rval().setNumber(str->getIndexValue());
        return true;
      }
    }
  }

  /* Step 1. */
  RootedString inputString(cx, ToString<CanGC>(cx, args[0]));
  if (!inputString) {
    return false;
  }
  args[0].setString(inputString);

  /* Steps 6-9. */
  bool stripPrefix = true;
  int32_t radix;
  if (!args.hasDefined(1)) {
    radix = 10;
  } else {
    if (!ToInt32(cx, args[1], &radix)) {
      return false;
    }
    if (radix == 0) {
      radix = 10;
    } else {
      if (radix < 2 || radix > 36) {
        args.rval().setNaN();
        return true;
      }
      if (radix != 16) {
        stripPrefix = false;
      }
    }
  }

  JSLinearString* linear = inputString->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  AutoCheckCannotGC nogc;
  size_t length = inputString->length();
  double number;
  if (linear->hasLatin1Chars()) {
    if (!ParseIntImpl(cx, linear->latin1Chars(nogc), length, stripPrefix, radix,
                      &number)) {
      return false;
    }
  } else {
    if (!ParseIntImpl(cx, linear->twoByteChars(nogc), length, stripPrefix,
                      radix, &number)) {
      return false;
    }
  }

  args.rval().setNumber(number);
  return true;
}

static const JSFunctionSpec number_functions[] = {
    JS_SELF_HOSTED_FN(js_isNaN_str, "Global_isNaN", 1, JSPROP_RESOLVING),
    JS_SELF_HOSTED_FN(js_isFinite_str, "Global_isFinite", 1, JSPROP_RESOLVING),
    JS_FS_END};

const JSClass NumberObject::class_ = {
    js_Number_str,
    JSCLASS_HAS_RESERVED_SLOTS(1) | JSCLASS_HAS_CACHED_PROTO(JSProto_Number),
    JS_NULL_CLASS_OPS, &NumberObject::classSpec_};

static bool Number(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  if (args.length() > 0) {
    // BigInt proposal section 6.2, steps 2a-c.
    if (!ToNumeric(cx, args[0])) {
      return false;
    }
    if (args[0].isBigInt()) {
      args[0].setNumber(BigInt::numberValue(args[0].toBigInt()));
    }
    MOZ_ASSERT(args[0].isNumber());
  }

  if (!args.isConstructing()) {
    if (args.length() > 0) {
      args.rval().set(args[0]);
    } else {
      args.rval().setInt32(0);
    }
    return true;
  }

  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Number, &proto)) {
    return false;
  }

  double d = args.length() > 0 ? args[0].toNumber() : 0;
  JSObject* obj = NumberObject::create(cx, d, proto);
  if (!obj) {
    return false;
  }
  args.rval().setObject(*obj);
  return true;
}

// ES2020 draft rev e08b018785606bc6465a0456a79604b149007932
// 20.1.3 Properties of the Number Prototype Object, thisNumberValue.
MOZ_ALWAYS_INLINE
static bool ThisNumberValue(JSContext* cx, const CallArgs& args,
                            const char* methodName, double* number) {
  HandleValue thisv = args.thisv();

  // Step 1.
  if (thisv.isNumber()) {
    *number = thisv.toNumber();
    return true;
  }

  // Steps 2-3.
  auto* obj = UnwrapAndTypeCheckThis<NumberObject>(cx, args, methodName);
  if (!obj) {
    return false;
  }

  *number = obj->unbox();
  return true;
}

// On-off helper function for the self-hosted Number_toLocaleString method.
// This only exists to produce an error message with the right method name.
bool js::ThisNumberValueForToLocaleString(JSContext* cx, unsigned argc,
                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  double d;
  if (!ThisNumberValue(cx, args, "toLocaleString", &d)) {
    return false;
  }

  args.rval().setNumber(d);
  return true;
}

static bool num_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  double d;
  if (!ThisNumberValue(cx, args, "toSource", &d)) {
    return false;
  }

  JSStringBuilder sb(cx);
  if (!sb.append("(new Number(") ||
      !NumberValueToStringBuffer(cx, NumberValue(d), sb) || !sb.append("))")) {
    return false;
  }

  JSString* str = sb.finishString();
  if (!str) {
    return false;
  }
  args.rval().setString(str);
  return true;
}

ToCStringBuf::ToCStringBuf() : dbuf(nullptr) {
  static_assert(sbufSize >= DTOSTR_STANDARD_BUFFER_SIZE,
                "builtin space must be large enough to store even the "
                "longest string produced by a conversion");
}

ToCStringBuf::~ToCStringBuf() { js_free(dbuf); }

MOZ_ALWAYS_INLINE
static JSLinearString* LookupDtoaCache(JSContext* cx, double d) {
  if (Realm* realm = cx->realm()) {
    if (JSLinearString* str = realm->dtoaCache.lookup(10, d)) {
      return str;
    }
  }

  return nullptr;
}

MOZ_ALWAYS_INLINE
static void CacheNumber(JSContext* cx, double d, JSLinearString* str) {
  if (Realm* realm = cx->realm()) {
    realm->dtoaCache.cache(10, d, str);
  }
}

MOZ_ALWAYS_INLINE
static JSLinearString* LookupInt32ToString(JSContext* cx, int32_t si) {
  if (si >= 0 && StaticStrings::hasInt(si)) {
    return cx->staticStrings().getInt(si);
  }

  return LookupDtoaCache(cx, si);
}

template <typename T>
MOZ_ALWAYS_INLINE static T* BackfillInt32InBuffer(int32_t si, T* buffer,
                                                  size_t size, size_t* length) {
  uint32_t ui = Abs(si);
  MOZ_ASSERT_IF(si == INT32_MIN, ui == uint32_t(INT32_MAX) + 1);

  RangedPtr<T> end(buffer + size - 1, buffer, size);
  *end = '\0';
  RangedPtr<T> start = BackfillIndexInCharBuffer(ui, end);
  if (si < 0) {
    *--start = '-';
  }

  *length = end - start;
  return start.get();
}

template <AllowGC allowGC>
JSLinearString* js::Int32ToString(JSContext* cx, int32_t si) {
  if (JSLinearString* str = LookupInt32ToString(cx, si)) {
    return str;
  }

  Latin1Char buffer[JSFatInlineString::MAX_LENGTH_LATIN1 + 1];
  size_t length;
  Latin1Char* start =
      BackfillInt32InBuffer(si, buffer, std::size(buffer), &length);

  mozilla::Range<const Latin1Char> chars(start, length);
  JSInlineString* str =
      NewInlineString<allowGC>(cx, chars, js::gc::DefaultHeap);
  if (!str) {
    return nullptr;
  }
  if (si >= 0) {
    str->maybeInitializeIndexValue(si);
  }

  CacheNumber(cx, si, str);
  return str;
}

template JSLinearString* js::Int32ToString<CanGC>(JSContext* cx, int32_t si);

template JSLinearString* js::Int32ToString<NoGC>(JSContext* cx, int32_t si);

JSLinearString* js::Int32ToStringPure(JSContext* cx, int32_t si) {
  AutoUnsafeCallWithABI unsafe;
  JSLinearString* res = Int32ToString<NoGC>(cx, si);
  if (!res) {
    cx->recoverFromOutOfMemory();
  }
  return res;
}

JSAtom* js::Int32ToAtom(JSContext* cx, int32_t si) {
  if (JSLinearString* str = LookupInt32ToString(cx, si)) {
    return js::AtomizeString(cx, str);
  }

  char buffer[JSFatInlineString::MAX_LENGTH_TWO_BYTE + 1];
  size_t length;
  char* start = BackfillInt32InBuffer(
      si, buffer, JSFatInlineString::MAX_LENGTH_TWO_BYTE + 1, &length);

  Maybe<uint32_t> indexValue;
  if (si >= 0) {
    indexValue.emplace(si);
  }

  JSAtom* atom = Atomize(cx, start, length, js::DoNotPinAtom, indexValue);
  if (!atom) {
    return nullptr;
  }

  CacheNumber(cx, si, atom);
  return atom;
}

frontend::TaggedParserAtomIndex js::Int32ToParserAtom(
    JSContext* cx, frontend::ParserAtomsTable& parserAtoms, int32_t si) {
  char buffer[JSFatInlineString::MAX_LENGTH_TWO_BYTE + 1];
  size_t length;
  char* start = BackfillInt32InBuffer(
      si, buffer, JSFatInlineString::MAX_LENGTH_TWO_BYTE + 1, &length);

  Maybe<uint32_t> indexValue;
  if (si >= 0) {
    indexValue.emplace(si);
  }

  return parserAtoms.internAscii(cx, start, length);
}

/* Returns a non-nullptr pointer to inside cbuf.  */
static char* Int32ToCString(ToCStringBuf* cbuf, int32_t i, size_t* len,
                            int base = 10) {
  uint32_t u = Abs(i);

  RangedPtr<char> cp(cbuf->sbuf + ToCStringBuf::sbufSize - 1, cbuf->sbuf,
                     ToCStringBuf::sbufSize);
  char* end = cp.get();
  *cp = '\0';

  /* Build the string from behind. */
  switch (base) {
    case 10:
      cp = BackfillIndexInCharBuffer(u, cp);
      break;
    case 16:
      do {
        unsigned newu = u / 16;
        *--cp = "0123456789abcdef"[u - newu * 16];
        u = newu;
      } while (u != 0);
      break;
    default:
      MOZ_ASSERT(base >= 2 && base <= 36);
      do {
        unsigned newu = u / base;
        *--cp = "0123456789abcdefghijklmnopqrstuvwxyz"[u - newu * base];
        u = newu;
      } while (u != 0);
      break;
  }
  if (i < 0) {
    *--cp = '-';
  }

  *len = end - cp.get();
  return cp.get();
}

template <AllowGC allowGC>
static JSString* NumberToStringWithBase(JSContext* cx, double d, int base);

static bool num_toString(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  double d;
  if (!ThisNumberValue(cx, args, "toString", &d)) {
    return false;
  }

  int32_t base = 10;
  if (args.hasDefined(0)) {
    double d2;
    if (!ToInteger(cx, args[0], &d2)) {
      return false;
    }

    if (d2 < 2 || d2 > 36) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_RADIX);
      return false;
    }

    base = int32_t(d2);
  }
  JSString* str = NumberToStringWithBase<CanGC>(cx, d, base);
  if (!str) {
    JS_ReportOutOfMemory(cx);
    return false;
  }
  args.rval().setString(str);
  return true;
}

#if !JS_HAS_INTL_API
static bool num_toLocaleString(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  double d;
  if (!ThisNumberValue(cx, args, "toLocaleString", &d)) {
    return false;
  }

  RootedString str(cx, NumberToStringWithBase<CanGC>(cx, d, 10));
  if (!str) {
    JS_ReportOutOfMemory(cx);
    return false;
  }

  /*
   * Create the string, move back to bytes to make string twiddling
   * a bit easier and so we can insert platform charset seperators.
   */
  UniqueChars numBytes = EncodeAscii(cx, str);
  if (!numBytes) {
    return false;
  }
  const char* num = numBytes.get();
  if (!num) {
    return false;
  }

  /*
   * Find the first non-integer value, whether it be a letter as in
   * 'Infinity', a decimal point, or an 'e' from exponential notation.
   */
  const char* nint = num;
  if (*nint == '-') {
    nint++;
  }
  while (*nint >= '0' && *nint <= '9') {
    nint++;
  }
  int digits = nint - num;
  const char* end = num + digits;
  if (!digits) {
    args.rval().setString(str);
    return true;
  }

  JSRuntime* rt = cx->runtime();
  size_t thousandsLength = strlen(rt->thousandsSeparator);
  size_t decimalLength = strlen(rt->decimalSeparator);

  /* Figure out how long resulting string will be. */
  int buflen = strlen(num);
  if (*nint == '.') {
    buflen += decimalLength - 1; /* -1 to account for existing '.' */
  }

  const char* numGrouping;
  const char* tmpGroup;
  numGrouping = tmpGroup = rt->numGrouping;
  int remainder = digits;
  if (*num == '-') {
    remainder--;
  }

  while (*tmpGroup != CHAR_MAX && *tmpGroup != '\0') {
    if (*tmpGroup >= remainder) {
      break;
    }
    buflen += thousandsLength;
    remainder -= *tmpGroup;
    tmpGroup++;
  }

  int nrepeat;
  if (*tmpGroup == '\0' && *numGrouping != '\0') {
    nrepeat = (remainder - 1) / tmpGroup[-1];
    buflen += thousandsLength * nrepeat;
    remainder -= nrepeat * tmpGroup[-1];
  } else {
    nrepeat = 0;
  }
  tmpGroup--;

  char* buf = cx->pod_malloc<char>(buflen + 1);
  if (!buf) {
    return false;
  }

  char* tmpDest = buf;
  const char* tmpSrc = num;

  while (*tmpSrc == '-' || remainder--) {
    MOZ_ASSERT(tmpDest - buf < buflen);
    *tmpDest++ = *tmpSrc++;
  }
  while (tmpSrc < end) {
    MOZ_ASSERT(tmpDest - buf + ptrdiff_t(thousandsLength) <= buflen);
    strcpy(tmpDest, rt->thousandsSeparator);
    tmpDest += thousandsLength;
    MOZ_ASSERT(tmpDest - buf + *tmpGroup <= buflen);
    js_memcpy(tmpDest, tmpSrc, *tmpGroup);
    tmpDest += *tmpGroup;
    tmpSrc += *tmpGroup;
    if (--nrepeat < 0) {
      tmpGroup--;
    }
  }

  if (*nint == '.') {
    MOZ_ASSERT(tmpDest - buf + ptrdiff_t(decimalLength) <= buflen);
    strcpy(tmpDest, rt->decimalSeparator);
    tmpDest += decimalLength;
    MOZ_ASSERT(tmpDest - buf + ptrdiff_t(strlen(nint + 1)) <= buflen);
    strcpy(tmpDest, nint + 1);
  } else {
    MOZ_ASSERT(tmpDest - buf + ptrdiff_t(strlen(nint)) <= buflen);
    strcpy(tmpDest, nint);
  }

  if (cx->runtime()->localeCallbacks &&
      cx->runtime()->localeCallbacks->localeToUnicode) {
    Rooted<Value> v(cx, StringValue(str));
    bool ok = !!cx->runtime()->localeCallbacks->localeToUnicode(cx, buf, &v);
    if (ok) {
      args.rval().set(v);
    }
    js_free(buf);
    return ok;
  }

  str = NewStringCopyN<CanGC>(cx, buf, buflen);
  js_free(buf);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}
#endif /* !JS_HAS_INTL_API */

bool js::num_valueOf(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  double d;
  if (!ThisNumberValue(cx, args, "valueOf", &d)) {
    return false;
  }

  args.rval().setNumber(d);
  return true;
}

static const unsigned MAX_PRECISION = 100;

static bool ComputePrecisionInRange(JSContext* cx, int minPrecision,
                                    int maxPrecision, double prec,
                                    int* precision) {
  if (minPrecision <= prec && prec <= maxPrecision) {
    *precision = int(prec);
    return true;
  }

  ToCStringBuf cbuf;
  if (char* numStr = NumberToCString(cx, &cbuf, prec, 10)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_PRECISION_RANGE, numStr);
  }
  return false;
}

static constexpr size_t DoubleToStrResultBufSize = 128;

template <typename Op>
[[nodiscard]] static bool DoubleToStrResult(JSContext* cx, const CallArgs& args,
                                            Op op) {
  char buf[DoubleToStrResultBufSize];

  const auto& converter =
      double_conversion::DoubleToStringConverter::EcmaScriptConverter();
  double_conversion::StringBuilder builder(buf, sizeof(buf));

  bool ok = op(converter, builder);
  MOZ_RELEASE_ASSERT(ok);

  const char* numStr = builder.Finalize();
  MOZ_ASSERT(numStr == buf);

  JSString* str = NewStringCopyZ<CanGC>(cx, numStr);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

// ES 2021 draft 21.1.3.3.
static bool num_toFixed(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  double d;
  if (!ThisNumberValue(cx, args, "toFixed", &d)) {
    return false;
  }

  // Steps 2-5.
  int precision;
  if (args.length() == 0) {
    precision = 0;
  } else {
    double prec = 0;
    if (!ToInteger(cx, args[0], &prec)) {
      return false;
    }

    if (!ComputePrecisionInRange(cx, 0, MAX_PRECISION, prec, &precision)) {
      return false;
    }
  }

  // Step 6.
  if (mozilla::IsNaN(d)) {
    args.rval().setString(cx->names().NaN);
    return true;
  }
  if (mozilla::IsInfinite(d)) {
    if (d > 0) {
      args.rval().setString(cx->names().Infinity);
      return true;
    }

    args.rval().setString(cx->names().NegativeInfinity);
    return true;
  }

  // Steps 7-10 for very large numbers.
  if (d <= -1e21 || d >= 1e+21) {
    JSString* s = NumberToString<CanGC>(cx, d);
    if (!s) {
      return false;
    }

    args.rval().setString(s);
    return true;
  }

  // Steps 7-12.

  // DoubleToStringConverter::ToFixed is documented as requiring a buffer size
  // of:
  //
  //   1 + kMaxFixedDigitsBeforePoint + 1 + kMaxFixedDigitsAfterPoint + 1
  //   (one additional character for the sign, one for the decimal point,
  //      and one for the null terminator)
  //
  // We already ensured there are at most 21 digits before the point, and
  // MAX_PRECISION digits after the point.
  static_assert(1 + 21 + 1 + MAX_PRECISION + 1 <= DoubleToStrResultBufSize);

  // The double-conversion library by default has a kMaxFixedDigitsAfterPoint of
  // 60. Assert our modified version supports at least MAX_PRECISION (100).
  using DToSConverter = double_conversion::DoubleToStringConverter;
  static_assert(DToSConverter::kMaxFixedDigitsAfterPoint >= MAX_PRECISION);

  return DoubleToStrResult(cx, args, [&](auto& converter, auto& builder) {
    return converter.ToFixed(d, precision, &builder);
  });
}

// ES 2021 draft 21.1.3.2.
static bool num_toExponential(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  double d;
  if (!ThisNumberValue(cx, args, "toExponential", &d)) {
    return false;
  }

  // Step 2.
  double prec = 0;
  if (args.hasDefined(0)) {
    if (!ToInteger(cx, args[0], &prec)) {
      return false;
    }
  }

  // Step 3.
  MOZ_ASSERT_IF(!args.hasDefined(0), prec == 0);

  // Step 4.
  if (mozilla::IsNaN(d)) {
    args.rval().setString(cx->names().NaN);
    return true;
  }
  if (mozilla::IsInfinite(d)) {
    if (d > 0) {
      args.rval().setString(cx->names().Infinity);
      return true;
    }

    args.rval().setString(cx->names().NegativeInfinity);
    return true;
  }

  // Step 5.
  int precision = 0;
  if (!ComputePrecisionInRange(cx, 0, MAX_PRECISION, prec, &precision)) {
    return false;
  }

  // Steps 6-15.

  // DoubleToStringConverter::ToExponential is documented as adding at most 8
  // characters on top of the requested digits: "the sign, the digit before the
  // decimal point, the decimal point, the exponent character, the exponent's
  // sign, and at most 3 exponent digits". In addition, the buffer must be able
  // to hold the trailing '\0' character.
  static_assert(MAX_PRECISION + 8 + 1 <= DoubleToStrResultBufSize);

  return DoubleToStrResult(cx, args, [&](auto& converter, auto& builder) {
    int requestedDigits = args.hasDefined(0) ? precision : -1;
    return converter.ToExponential(d, requestedDigits, &builder);
  });
}

// ES 2021 draft 21.1.3.5.
static bool num_toPrecision(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  double d;
  if (!ThisNumberValue(cx, args, "toPrecision", &d)) {
    return false;
  }

  // Step 2.
  if (!args.hasDefined(0)) {
    JSString* str = NumberToStringWithBase<CanGC>(cx, d, 10);
    if (!str) {
      JS_ReportOutOfMemory(cx);
      return false;
    }
    args.rval().setString(str);
    return true;
  }

  // Step 3.
  double prec = 0;
  if (!ToInteger(cx, args[0], &prec)) {
    return false;
  }

  // Step 4.
  if (mozilla::IsNaN(d)) {
    args.rval().setString(cx->names().NaN);
    return true;
  }
  if (mozilla::IsInfinite(d)) {
    if (d > 0) {
      args.rval().setString(cx->names().Infinity);
      return true;
    }

    args.rval().setString(cx->names().NegativeInfinity);
    return true;
  }

  // Step 5.
  int precision = 0;
  if (!ComputePrecisionInRange(cx, 1, MAX_PRECISION, prec, &precision)) {
    return false;
  }

  // Steps 6-14.

  // DoubleToStringConverter::ToPrecision is documented as adding at most 7
  // characters on top of the requested digits: "the sign, the decimal point,
  // the exponent character, the exponent's sign, and at most 3 exponent
  // digits". In addition, the buffer must be able to hold the trailing '\0'
  // character.
  static_assert(MAX_PRECISION + 7 + 1 <= DoubleToStrResultBufSize);

  return DoubleToStrResult(cx, args, [&](auto& converter, auto& builder) {
    return converter.ToPrecision(d, precision, &builder);
  });
}

static const JSFunctionSpec number_methods[] = {
    JS_FN(js_toSource_str, num_toSource, 0, 0),
    JS_INLINABLE_FN(js_toString_str, num_toString, 1, 0, NumberToString),
#if JS_HAS_INTL_API
    JS_SELF_HOSTED_FN(js_toLocaleString_str, "Number_toLocaleString", 0, 0),
#else
    JS_FN(js_toLocaleString_str, num_toLocaleString, 0, 0),
#endif
    JS_FN(js_valueOf_str, num_valueOf, 0, 0),
    JS_FN("toFixed", num_toFixed, 1, 0),
    JS_FN("toExponential", num_toExponential, 1, 0),
    JS_FN("toPrecision", num_toPrecision, 1, 0),
    JS_FS_END};

bool js::IsInteger(const Value& val) {
  return val.isInt32() || IsInteger(val.toDouble());
}

bool js::IsInteger(double d) {
  return mozilla::IsFinite(d) && JS::ToInteger(d) == d;
}

static const JSFunctionSpec number_static_methods[] = {
    JS_SELF_HOSTED_FN("isFinite", "Number_isFinite", 1, 0),
    JS_SELF_HOSTED_FN("isInteger", "Number_isInteger", 1, 0),
    JS_SELF_HOSTED_FN("isNaN", "Number_isNaN", 1, 0),
    JS_SELF_HOSTED_FN("isSafeInteger", "Number_isSafeInteger", 1, 0),
    JS_FS_END};

static const JSPropertySpec number_static_properties[] = {
    JS_DOUBLE_PS("POSITIVE_INFINITY", mozilla::PositiveInfinity<double>(),
                 JSPROP_READONLY | JSPROP_PERMANENT),
    JS_DOUBLE_PS("NEGATIVE_INFINITY", mozilla::NegativeInfinity<double>(),
                 JSPROP_READONLY | JSPROP_PERMANENT),
    JS_DOUBLE_PS("MAX_VALUE", 1.7976931348623157E+308,
                 JSPROP_READONLY | JSPROP_PERMANENT),
    JS_DOUBLE_PS("MIN_VALUE", MinNumberValue<double>(),
                 JSPROP_READONLY | JSPROP_PERMANENT),
    /* ES6 (April 2014 draft) 20.1.2.6 */
    JS_DOUBLE_PS("MAX_SAFE_INTEGER", 9007199254740991,
                 JSPROP_READONLY | JSPROP_PERMANENT),
    /* ES6 (April 2014 draft) 20.1.2.10 */
    JS_DOUBLE_PS("MIN_SAFE_INTEGER", -9007199254740991,
                 JSPROP_READONLY | JSPROP_PERMANENT),
    /* ES6 (May 2013 draft) 15.7.3.7 */
    JS_DOUBLE_PS("EPSILON", 2.2204460492503130808472633361816e-16,
                 JSPROP_READONLY | JSPROP_PERMANENT),
    JS_PS_END};

bool js::InitRuntimeNumberState(JSRuntime* rt) {
  // XXX If JS_HAS_INTL_API becomes true all the time at some point,
  //     js::InitRuntimeNumberState is no longer fallible, and we should
  //     change its return type.
#if !JS_HAS_INTL_API
  /* Copy locale-specific separators into the runtime strings. */
  const char* thousandsSeparator;
  const char* decimalPoint;
  const char* grouping;
#  ifdef HAVE_LOCALECONV
  struct lconv* locale = localeconv();
  thousandsSeparator = locale->thousands_sep;
  decimalPoint = locale->decimal_point;
  grouping = locale->grouping;
#  else
  thousandsSeparator = getenv("LOCALE_THOUSANDS_SEP");
  decimalPoint = getenv("LOCALE_DECIMAL_POINT");
  grouping = getenv("LOCALE_GROUPING");
#  endif
  if (!thousandsSeparator) {
    thousandsSeparator = "'";
  }
  if (!decimalPoint) {
    decimalPoint = ".";
  }
  if (!grouping) {
    grouping = "\3\0";
  }

  /*
   * We use single malloc to get the memory for all separator and grouping
   * strings.
   */
  size_t thousandsSeparatorSize = strlen(thousandsSeparator) + 1;
  size_t decimalPointSize = strlen(decimalPoint) + 1;
  size_t groupingSize = strlen(grouping) + 1;

  char* storage = js_pod_malloc<char>(thousandsSeparatorSize +
                                      decimalPointSize + groupingSize);
  if (!storage) {
    return false;
  }

  js_memcpy(storage, thousandsSeparator, thousandsSeparatorSize);
  rt->thousandsSeparator = storage;
  storage += thousandsSeparatorSize;

  js_memcpy(storage, decimalPoint, decimalPointSize);
  rt->decimalSeparator = storage;
  storage += decimalPointSize;

  js_memcpy(storage, grouping, groupingSize);
  rt->numGrouping = grouping;
#endif /* !JS_HAS_INTL_API */
  return true;
}

void js::FinishRuntimeNumberState(JSRuntime* rt) {
#if !JS_HAS_INTL_API
  /*
   * The free also releases the memory for decimalSeparator and numGrouping
   * strings.
   */
  char* storage = const_cast<char*>(rt->thousandsSeparator.ref());
  js_free(storage);
#endif  // !JS_HAS_INTL_API
}

JSObject* NumberObject::createPrototype(JSContext* cx, JSProtoKey key) {
  NumberObject* numberProto =
      GlobalObject::createBlankPrototype<NumberObject>(cx, cx->global());
  if (!numberProto) {
    return nullptr;
  }
  numberProto->setPrimitiveValue(0);
  return numberProto;
}

static bool NumberClassFinish(JSContext* cx, HandleObject ctor,
                              HandleObject proto) {
  Handle<GlobalObject*> global = cx->global();

  if (!JS_DefineFunctions(cx, global, number_functions)) {
    return false;
  }

  // Number.parseInt should be the same function object as global parseInt.
  RootedId parseIntId(cx, NameToId(cx->names().parseInt));
  JSFunction* parseInt =
      DefineFunction(cx, global, parseIntId, num_parseInt, 2, JSPROP_RESOLVING);
  if (!parseInt) {
    return false;
  }
  RootedValue parseIntValue(cx, ObjectValue(*parseInt));
  if (!DefineDataProperty(cx, ctor, parseIntId, parseIntValue, 0)) {
    return false;
  }

  // Number.parseFloat should be the same function object as global
  // parseFloat.
  RootedId parseFloatId(cx, NameToId(cx->names().parseFloat));
  JSFunction* parseFloat = DefineFunction(cx, global, parseFloatId,
                                          num_parseFloat, 1, JSPROP_RESOLVING);
  if (!parseFloat) {
    return false;
  }
  RootedValue parseFloatValue(cx, ObjectValue(*parseFloat));
  if (!DefineDataProperty(cx, ctor, parseFloatId, parseFloatValue, 0)) {
    return false;
  }

  RootedValue valueNaN(cx, JS::NaNValue());
  RootedValue valueInfinity(cx, JS::InfinityValue());

  if (!DefineDataProperty(
          cx, ctor, cx->names().NaN, valueNaN,
          JSPROP_PERMANENT | JSPROP_READONLY | JSPROP_RESOLVING)) {
    return false;
  }

  // ES5 15.1.1.1, 15.1.1.2
  if (!NativeDefineDataProperty(
          cx, global, cx->names().NaN, valueNaN,
          JSPROP_PERMANENT | JSPROP_READONLY | JSPROP_RESOLVING) ||
      !NativeDefineDataProperty(
          cx, global, cx->names().Infinity, valueInfinity,
          JSPROP_PERMANENT | JSPROP_READONLY | JSPROP_RESOLVING)) {
    return false;
  }

  return true;
}

const ClassSpec NumberObject::classSpec_ = {
    GenericCreateConstructor<Number, 1, gc::AllocKind::FUNCTION>,
    NumberObject::createPrototype,
    number_static_methods,
    number_static_properties,
    number_methods,
    nullptr,
    NumberClassFinish};

static char* FracNumberToCString(JSContext* cx, ToCStringBuf* cbuf, double d,
                                 int base = 10) {
#ifdef DEBUG
  {
    int32_t _;
    MOZ_ASSERT(!NumberEqualsInt32(d, &_));
  }
#endif

  char* numStr;
  if (base == 10) {
    /*
     * This is V8's implementation of the algorithm described in the
     * following paper:
     *
     *   Printing floating-point numbers quickly and accurately with integers.
     *   Florian Loitsch, PLDI 2010.
     */
    const double_conversion::DoubleToStringConverter& converter =
        double_conversion::DoubleToStringConverter::EcmaScriptConverter();
    double_conversion::StringBuilder builder(cbuf->sbuf,
                                             js::ToCStringBuf::sbufSize);
    converter.ToShortest(d, &builder);
    numStr = builder.Finalize();
  } else {
    if (!EnsureDtoaState(cx)) {
      return nullptr;
    }
    numStr = cbuf->dbuf = js_dtobasestr(cx->dtoaState, base, d);
  }
  return numStr;
}

void JS::NumberToString(double d, char (&out)[MaximumNumberToStringLength]) {
  int32_t i;
  if (NumberEqualsInt32(d, &i)) {
    ToCStringBuf cbuf;
    size_t len;
    char* loc = Int32ToCString(&cbuf, i, &len, 10);
    memmove(out, loc, len);
    out[len] = '\0';
  } else {
    const double_conversion::DoubleToStringConverter& converter =
        double_conversion::DoubleToStringConverter::EcmaScriptConverter();

    double_conversion::StringBuilder builder(out, sizeof(out));
    converter.ToShortest(d, &builder);

#ifdef DEBUG
    char* result =
#endif
        builder.Finalize();
    MOZ_ASSERT(out == result);
  }
}

char* js::NumberToCString(JSContext* cx, ToCStringBuf* cbuf, double d,
                          int base /* = 10*/) {
  int32_t i;
  size_t len;
  return NumberEqualsInt32(d, &i) ? Int32ToCString(cbuf, i, &len, base)
                                  : FracNumberToCString(cx, cbuf, d, base);
}

template <AllowGC allowGC>
static JSString* NumberToStringWithBase(JSContext* cx, double d, int base) {
  MOZ_ASSERT(2 <= base && base <= 36);

  ToCStringBuf cbuf;
  char* numStr;
  size_t numStrLen;

  Realm* realm = cx->realm();

  int32_t i;
  bool isBase10Int = false;
  if (NumberEqualsInt32(d, &i)) {
    isBase10Int = (base == 10);
    if (isBase10Int && StaticStrings::hasInt(i)) {
      return cx->staticStrings().getInt(i);
    }
    if (unsigned(i) < unsigned(base)) {
      if (i < 10) {
        return cx->staticStrings().getInt(i);
      }
      char16_t c = 'a' + i - 10;
      MOZ_ASSERT(StaticStrings::hasUnit(c));
      return cx->staticStrings().getUnit(c);
    }
    if (unsigned(i) < unsigned(base * base)) {
      static constexpr char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
      char chars[] = {digits[i / base], digits[i % base]};
      JSString* str = cx->staticStrings().lookup(chars, 2);
      MOZ_ASSERT(str);
      return str;
    }

    if (JSLinearString* str = realm->dtoaCache.lookup(base, d)) {
      return str;
    }

    numStr = Int32ToCString(&cbuf, i, &numStrLen, base);
    MOZ_ASSERT(!cbuf.dbuf && numStr >= cbuf.sbuf &&
               numStr < cbuf.sbuf + cbuf.sbufSize);
    MOZ_ASSERT(numStrLen == strlen(numStr));
  } else {
    if (JSLinearString* str = realm->dtoaCache.lookup(base, d)) {
      return str;
    }

    numStr = FracNumberToCString(cx, &cbuf, d, base);
    if (!numStr) {
      ReportOutOfMemory(cx);
      return nullptr;
    }
    MOZ_ASSERT_IF(base == 10, !cbuf.dbuf && numStr >= cbuf.sbuf &&
                                  numStr < cbuf.sbuf + cbuf.sbufSize);
    MOZ_ASSERT_IF(base != 10, cbuf.dbuf && cbuf.dbuf == numStr);

    numStrLen = strlen(numStr);
  }

  JSLinearString* s =
      NewStringCopyN<allowGC>(cx, numStr, numStrLen, js::gc::DefaultHeap);
  if (!s) {
    return nullptr;
  }

  if (isBase10Int && i >= 0) {
    s->maybeInitializeIndexValue(i);
  }

  realm->dtoaCache.cache(base, d, s);
  return s;
}

template <AllowGC allowGC>
JSString* js::NumberToString(JSContext* cx, double d) {
  return NumberToStringWithBase<allowGC>(cx, d, 10);
}

template JSString* js::NumberToString<CanGC>(JSContext* cx, double d);

template JSString* js::NumberToString<NoGC>(JSContext* cx, double d);

JSString* js::NumberToStringPure(JSContext* cx, double d) {
  AutoUnsafeCallWithABI unsafe;
  JSString* res = NumberToString<NoGC>(cx, d);
  if (!res) {
    cx->recoverFromOutOfMemory();
  }
  return res;
}

JSAtom* js::NumberToAtom(JSContext* cx, double d) {
  int32_t si;
  if (NumberEqualsInt32(d, &si)) {
    return Int32ToAtom(cx, si);
  }

  if (JSLinearString* str = LookupDtoaCache(cx, d)) {
    return AtomizeString(cx, str);
  }

  ToCStringBuf cbuf;
  char* numStr = FracNumberToCString(cx, &cbuf, d);
  if (!numStr) {
    ReportOutOfMemory(cx);
    return nullptr;
  }
  MOZ_ASSERT(!cbuf.dbuf && numStr >= cbuf.sbuf &&
             numStr < cbuf.sbuf + cbuf.sbufSize);

  size_t length = strlen(numStr);
  JSAtom* atom = Atomize(cx, numStr, length);
  if (!atom) {
    return nullptr;
  }

  CacheNumber(cx, d, atom);

  return atom;
}

frontend::TaggedParserAtomIndex js::NumberToParserAtom(
    JSContext* cx, frontend::ParserAtomsTable& parserAtoms, double d) {
  int32_t si;
  if (NumberEqualsInt32(d, &si)) {
    return Int32ToParserAtom(cx, parserAtoms, si);
  }

  ToCStringBuf cbuf;
  char* numStr = FracNumberToCString(cx, &cbuf, d);
  if (!numStr) {
    ReportOutOfMemory(cx);
    return frontend::TaggedParserAtomIndex::null();
  }
  MOZ_ASSERT(!cbuf.dbuf && numStr >= cbuf.sbuf &&
             numStr < cbuf.sbuf + cbuf.sbufSize);

  size_t length = strlen(numStr);
  return parserAtoms.internAscii(cx, numStr, length);
}

JSLinearString* js::IndexToString(JSContext* cx, uint32_t index) {
  if (StaticStrings::hasUint(index)) {
    return cx->staticStrings().getUint(index);
  }

  Realm* realm = cx->realm();
  if (JSLinearString* str = realm->dtoaCache.lookup(10, index)) {
    return str;
  }

  Latin1Char buffer[JSFatInlineString::MAX_LENGTH_LATIN1 + 1];
  RangedPtr<Latin1Char> end(buffer + JSFatInlineString::MAX_LENGTH_LATIN1,
                            buffer, JSFatInlineString::MAX_LENGTH_LATIN1 + 1);
  *end = '\0';
  RangedPtr<Latin1Char> start = BackfillIndexInCharBuffer(index, end);

  mozilla::Range<const Latin1Char> chars(start.get(), end - start);
  JSInlineString* str = NewInlineString<CanGC>(cx, chars, js::gc::DefaultHeap);
  if (!str) {
    return nullptr;
  }

  realm->dtoaCache.cache(10, index, str);
  return str;
}

bool js::NumberValueToStringBuffer(JSContext* cx, const Value& v,
                                   StringBuffer& sb) {
  /* Convert to C-string. */
  ToCStringBuf cbuf;
  const char* cstr;
  size_t cstrlen;
  if (v.isInt32()) {
    cstr = Int32ToCString(&cbuf, v.toInt32(), &cstrlen);
    MOZ_ASSERT(cstrlen == strlen(cstr));
  } else {
    cstr = NumberToCString(cx, &cbuf, v.toDouble());
    if (!cstr) {
      JS_ReportOutOfMemory(cx);
      return false;
    }
    cstrlen = strlen(cstr);
  }

  MOZ_ASSERT(!cbuf.dbuf && cstrlen < cbuf.sbufSize);
  return sb.append(cstr, cstrlen);
}

template <typename CharT>
inline void CharToNumber(CharT c, double* result) {
  if ('0' <= c && c <= '9') {
    *result = c - '0';
  } else if (unicode::IsSpace(c)) {
    *result = 0.0;
  } else {
    *result = GenericNaN();
  }
}

template <typename CharT>
inline bool CharsToNonDecimalNumber(const CharT* start, const CharT* end,
                                    double* result) {
  MOZ_ASSERT(end - start >= 2);
  MOZ_ASSERT(start[0] == '0');

  int radix = 0;
  if (start[1] == 'b' || start[1] == 'B') {
    radix = 2;
  } else if (start[1] == 'o' || start[1] == 'O') {
    radix = 8;
  } else if (start[1] == 'x' || start[1] == 'X') {
    radix = 16;
  } else {
    return false;
  }

  // It's probably a non-decimal number. Accept if there's at least one digit
  // after the 0b|0o|0x, and if no non-whitespace characters follow all the
  // digits.
  const CharT* endptr;
  double d;
  MOZ_ALWAYS_TRUE(GetPrefixInteger(
      start + 2, end, radix, IntegerSeparatorHandling::None, &endptr, &d));
  if (endptr == start + 2 || SkipSpace(endptr, end) != end) {
    *result = GenericNaN();
  } else {
    *result = d;
  }
  return true;
}

template <typename CharT>
bool js::CharsToNumber(JSContext* cx, const CharT* chars, size_t length,
                       double* result) {
  if (length == 1) {
    CharToNumber(chars[0], result);
    return true;
  }

  const CharT* end = chars + length;
  const CharT* start = SkipSpace(chars, end);

  // ECMA doesn't allow signed non-decimal numbers (bug 273467).
  if (end - start >= 2 && start[0] == '0') {
    if (CharsToNonDecimalNumber(start, end, result)) {
      return true;
    }
  }

  /*
   * Note that ECMA doesn't treat a string beginning with a '0' as
   * an octal number here. This works because all such numbers will
   * be interpreted as decimal by js_strtod.  Also, any hex numbers
   * that have made it here (which can only be negative ones) will
   * be treated as 0 without consuming the 'x' by js_strtod.
   */
  const CharT* ep;
  double d;
  if (!js_strtod(cx, start, end, &ep, &d)) {
    *result = GenericNaN();
    return false;
  }

  if (SkipSpace(ep, end) != end) {
    *result = GenericNaN();
  } else {
    *result = d;
  }

  return true;
}

template bool js::CharsToNumber(JSContext* cx, const Latin1Char* chars,
                                size_t length, double* result);

template bool js::CharsToNumber(JSContext* cx, const char16_t* chars,
                                size_t length, double* result);

template <typename CharT>
static bool CharsToNumber(const CharT* chars, size_t length, double* result) {
  if (length == 1) {
    CharToNumber(chars[0], result);
    return true;
  }

  const CharT* end = chars + length;
  const CharT* start = SkipSpace(chars, end);

  // ECMA doesn't allow signed non-decimal numbers (bug 273467).
  if (end - start >= 2 && start[0] == '0') {
    if (CharsToNonDecimalNumber(start, end, result)) {
      return true;
    }
  }

  // It's probably a decimal number. Accept if no non-whitespace characters
  // follow all the digits.
  //
  // NB: Fractional digits are not supported, because they require calling into
  // dtoa, which isn't possible without a JSContext.
  const CharT* endptr;
  double d;
  if (!GetPrefixInteger(start, end, 10, IntegerSeparatorHandling::None, &endptr,
                        &d) ||
      SkipSpace(endptr, end) != end) {
    return false;
  }

  *result = d;
  return true;
}

bool js::StringToNumber(JSContext* cx, JSString* str, double* result) {
  AutoCheckCannotGC nogc;
  JSLinearString* linearStr = str->ensureLinear(cx);
  if (!linearStr) {
    return false;
  }

  if (str->hasIndexValue()) {
    *result = str->getIndexValue();
    return true;
  }

  return linearStr->hasLatin1Chars()
             ? CharsToNumber(cx, linearStr->latin1Chars(nogc), str->length(),
                             result)
             : CharsToNumber(cx, linearStr->twoByteChars(nogc), str->length(),
                             result);
}

bool js::StringToNumberPure(JSContext* cx, JSString* str, double* result) {
  // IC Code calls this directly.
  AutoUnsafeCallWithABI unsafe;

  if (!StringToNumber(cx, str, result)) {
    cx->recoverFromOutOfMemory();
    return false;
  }
  return true;
}

bool js::MaybeStringToNumber(JSLinearString* str, double* result) {
  AutoCheckCannotGC nogc;

  if (str->hasIndexValue()) {
    *result = str->getIndexValue();
    return true;
  }

  return str->hasLatin1Chars()
             ? ::CharsToNumber(str->latin1Chars(nogc), str->length(), result)
             : ::CharsToNumber(str->twoByteChars(nogc), str->length(), result);
}

JS_PUBLIC_API bool js::ToNumberSlow(JSContext* cx, HandleValue v_,
                                    double* out) {
  RootedValue v(cx, v_);
  MOZ_ASSERT(!v.isNumber());

  if (!v.isPrimitive()) {
    if (cx->isHelperThreadContext()) {
      return false;
    }

    if (!ToPrimitive(cx, JSTYPE_NUMBER, &v)) {
      return false;
    }

    if (v.isNumber()) {
      *out = v.toNumber();
      return true;
    }
  }
  if (v.isString()) {
    return StringToNumber(cx, v.toString(), out);
  }
  if (v.isBoolean()) {
    *out = v.toBoolean() ? 1.0 : 0.0;
    return true;
  }
  if (v.isNull()) {
    *out = 0.0;
    return true;
  }

  if (v.isUndefined()) {
    *out = GenericNaN();
    return true;
  }

  MOZ_ASSERT(v.isSymbol() || v.isBigInt());
  if (!cx->isHelperThreadContext()) {
    unsigned errnum = JSMSG_SYMBOL_TO_NUMBER;
    if (v.isBigInt()) {
      errnum = JSMSG_BIGINT_TO_NUMBER;
    }
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, errnum);
  }
  return false;
}

// BigInt proposal section 3.1.6
bool js::ToNumericSlow(JSContext* cx, MutableHandleValue vp) {
  MOZ_ASSERT(!vp.isNumeric());

  // Step 1.
  if (!vp.isPrimitive()) {
    if (cx->isHelperThreadContext()) {
      return false;
    }
    if (!ToPrimitive(cx, JSTYPE_NUMBER, vp)) {
      return false;
    }
  }

  // Step 2.
  if (vp.isBigInt()) {
    return true;
  }

  // Step 3.
  return ToNumber(cx, vp);
}

/*
 * Convert a value to an int8_t, according to the WebIDL rules for byte
 * conversion. Return converted value in *out on success, false on failure.
 */
JS_PUBLIC_API bool js::ToInt8Slow(JSContext* cx, const HandleValue v,
                                  int8_t* out) {
  MOZ_ASSERT(!v.isInt32());
  double d;
  if (v.isDouble()) {
    d = v.toDouble();
  } else {
    if (!ToNumberSlow(cx, v, &d)) {
      return false;
    }
  }
  *out = ToInt8(d);
  return true;
}

/*
 * Convert a value to an uint8_t, according to the ToUInt8() function in ES6
 * ECMA-262, 7.1.10. Return converted value in *out on success, false on
 * failure.
 */
JS_PUBLIC_API bool js::ToUint8Slow(JSContext* cx, const HandleValue v,
                                   uint8_t* out) {
  MOZ_ASSERT(!v.isInt32());
  double d;
  if (v.isDouble()) {
    d = v.toDouble();
  } else {
    if (!ToNumberSlow(cx, v, &d)) {
      return false;
    }
  }
  *out = ToUint8(d);
  return true;
}

/*
 * Convert a value to an int16_t, according to the WebIDL rules for short
 * conversion. Return converted value in *out on success, false on failure.
 */
JS_PUBLIC_API bool js::ToInt16Slow(JSContext* cx, const HandleValue v,
                                   int16_t* out) {
  MOZ_ASSERT(!v.isInt32());
  double d;
  if (v.isDouble()) {
    d = v.toDouble();
  } else {
    if (!ToNumberSlow(cx, v, &d)) {
      return false;
    }
  }
  *out = ToInt16(d);
  return true;
}

/*
 * Convert a value to an int64_t, according to the WebIDL rules for long long
 * conversion. Return converted value in *out on success, false on failure.
 */
JS_PUBLIC_API bool js::ToInt64Slow(JSContext* cx, const HandleValue v,
                                   int64_t* out) {
  MOZ_ASSERT(!v.isInt32());
  double d;
  if (v.isDouble()) {
    d = v.toDouble();
  } else {
    if (!ToNumberSlow(cx, v, &d)) {
      return false;
    }
  }
  *out = ToInt64(d);
  return true;
}

/*
 * Convert a value to an uint64_t, according to the WebIDL rules for unsigned
 * long long conversion. Return converted value in *out on success, false on
 * failure.
 */
JS_PUBLIC_API bool js::ToUint64Slow(JSContext* cx, const HandleValue v,
                                    uint64_t* out) {
  MOZ_ASSERT(!v.isInt32());
  double d;
  if (v.isDouble()) {
    d = v.toDouble();
  } else {
    if (!ToNumberSlow(cx, v, &d)) {
      return false;
    }
  }
  *out = ToUint64(d);
  return true;
}

JS_PUBLIC_API bool js::ToInt32Slow(JSContext* cx, const HandleValue v,
                                   int32_t* out) {
  MOZ_ASSERT(!v.isInt32());
  double d;
  if (v.isDouble()) {
    d = v.toDouble();
  } else {
    if (!ToNumberSlow(cx, v, &d)) {
      return false;
    }
  }
  *out = ToInt32(d);
  return true;
}

bool js::ToInt32OrBigIntSlow(JSContext* cx, MutableHandleValue vp) {
  MOZ_ASSERT(!vp.isInt32());
  if (vp.isDouble()) {
    vp.setInt32(ToInt32(vp.toDouble()));
    return true;
  }

  if (!ToNumeric(cx, vp)) {
    return false;
  }

  if (vp.isBigInt()) {
    return true;
  }

  vp.setInt32(ToInt32(vp.toNumber()));
  return true;
}

JS_PUBLIC_API bool js::ToUint32Slow(JSContext* cx, const HandleValue v,
                                    uint32_t* out) {
  MOZ_ASSERT(!v.isInt32());
  double d;
  if (v.isDouble()) {
    d = v.toDouble();
  } else {
    if (!ToNumberSlow(cx, v, &d)) {
      return false;
    }
  }
  *out = ToUint32(d);
  return true;
}

JS_PUBLIC_API bool js::ToUint16Slow(JSContext* cx, const HandleValue v,
                                    uint16_t* out) {
  MOZ_ASSERT(!v.isInt32());
  double d;
  if (v.isDouble()) {
    d = v.toDouble();
  } else if (!ToNumberSlow(cx, v, &d)) {
    return false;
  }
  *out = ToUint16(d);
  return true;
}

// ES2017 draft 7.1.17 ToIndex
bool js::ToIndexSlow(JSContext* cx, JS::HandleValue v,
                     const unsigned errorNumber, uint64_t* index) {
  MOZ_ASSERT_IF(v.isInt32(), v.toInt32() < 0);

  // Step 1.
  if (v.isUndefined()) {
    *index = 0;
    return true;
  }

  // Step 2.a.
  double integerIndex;
  if (!ToInteger(cx, v, &integerIndex)) {
    return false;
  }

  // Inlined version of ToLength.
  // 1. Already an integer.
  // 2. Step eliminates < 0, +0 == -0 with SameValueZero.
  // 3/4. Limit to <= 2^53-1, so everything above should fail.
  if (integerIndex < 0 || integerIndex >= DOUBLE_INTEGRAL_PRECISION_LIMIT) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, errorNumber);
    return false;
  }

  // Step 3.
  *index = uint64_t(integerIndex);
  return true;
}

template <typename CharT>
bool js_strtod(JSContext* cx, const CharT* begin, const CharT* end,
               const CharT** dEnd, double* d) {
  const CharT* s = SkipSpace(begin, end);
  size_t length = end - s;

  Vector<char, 32> chars(cx);
  if (!chars.growByUninitialized(length + 1)) {
    return false;
  }

  size_t i = 0;
  for (; i < length; i++) {
    char16_t c = s[i];
    if (c >> 8) {
      break;
    }
    chars[i] = char(c);
  }
  chars[i] = 0;

  /* Try to parse +Infinity, -Infinity or Infinity. */
  {
    char* afterSign = chars.begin();
    bool negative = (*afterSign == '-');
    if (negative || *afterSign == '+') {
      afterSign++;
    }

    if (*afterSign == 'I' && !strncmp(afterSign, "Infinity", 8)) {
      *d = negative ? NegativeInfinity<double>() : PositiveInfinity<double>();
      *dEnd = s + (afterSign - chars.begin()) + 8;
      return true;
    }
  }

  if (!EnsureDtoaState(cx)) {
    return false;
  }

  /* Everything else. */
  char* ep;
  *d = js_strtod_harder(cx->dtoaState, chars.begin(), &ep);

  MOZ_ASSERT(ep >= chars.begin());

  if (ep == chars.begin()) {
    *dEnd = begin;
  } else {
    *dEnd = s + (ep - chars.begin());
  }

  return true;
}

template bool js_strtod(JSContext* cx, const char16_t* begin,
                        const char16_t* end, const char16_t** dEnd, double* d);

template bool js_strtod(JSContext* cx, const Latin1Char* begin,
                        const Latin1Char* end, const Latin1Char** dEnd,
                        double* d);
