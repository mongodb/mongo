/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS number type and wrapper class.
 */

#include "jsnum.h"

#include "mozilla/Casting.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Maybe.h"
#include "mozilla/RangedPtr.h"
#include "mozilla/TextUtils.h"
#include "mozilla/Utf8.h"

#include <algorithm>
#include <iterator>
#include <limits>
#ifdef HAVE_LOCALECONV
#  include <locale.h>
#endif
#include <math.h>
#include <string.h>  // memmove
#include <string_view>

#include "jstypes.h"

#include "builtin/String.h"
#include "double-conversion/double-conversion.h"
#include "frontend/ParserAtom.h"  // frontend::{ParserAtomsTable, TaggedParserAtomIndex}
#include "jit/InlinableNatives.h"
#include "js/CharacterEncoding.h"
#include "js/Conversions.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/GCAPI.h"
#if !JS_HAS_INTL_API
#  include "js/LocaleSensitive.h"
#endif
#include "js/PropertyAndElement.h"  // JS_DefineFunctions
#include "js/PropertySpec.h"
#include "util/DoubleToString.h"
#include "util/Memory.h"
#include "util/StringBuffer.h"
#include "vm/BigIntType.h"
#include "vm/GlobalObject.h"
#include "vm/JSAtomUtils.h"  // Atomize, AtomizeString
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/StaticStrings.h"

#include "vm/Compartment-inl.h"  // For js::UnwrapAndTypeCheckThis
#include "vm/GeckoProfiler-inl.h"
#include "vm/JSAtomUtils-inl.h"  // BackfillIndexInCharBuffer
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
static bool GetPrefixIntegerImpl(const CharT* start, const CharT* end, int base,
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
bool js::GetPrefixInteger(const CharT* start, const CharT* end, int base,
                          IntegerSeparatorHandling separatorHandling,
                          const CharT** endp, double* dp) {
  if (GetPrefixIntegerImpl(start, end, base, separatorHandling, endp, dp)) {
    return true;
  }

  // Can only fail for base 10.
  MOZ_ASSERT(base == 10);

  // If we're accumulating a decimal number and the number is >= 2^53, then the
  // fast result from the loop in GetPrefixIntegerImpl may be inaccurate. Call
  // GetDecimal to get the correct answer.
  return GetDecimal(start, *endp, dp);
}

namespace js {

template bool GetPrefixInteger(const char16_t* start, const char16_t* end,
                               int base,
                               IntegerSeparatorHandling separatorHandling,
                               const char16_t** endp, double* dp);

template bool GetPrefixInteger(const Latin1Char* start, const Latin1Char* end,
                               int base,
                               IntegerSeparatorHandling separatorHandling,
                               const Latin1Char** endp, double* dp);

}  // namespace js

template <typename CharT>
bool js::GetDecimalInteger(const CharT* start, const CharT* end, double* dp) {
  MOZ_ASSERT(start <= end);

  double d = 0.0;
  for (const CharT* s = start; s < end; s++) {
    CharT c = *s;
    if (c == '_') {
      AssertWellPlacedNumericSeparator(s, start, end);
      continue;
    }
    MOZ_ASSERT(IsAsciiDigit(c));
    int digit = c - '0';
    d = d * 10 + digit;
  }

  // If we haven't reached the limit of integer precision, we're done.
  if (d < DOUBLE_INTEGRAL_PRECISION_LIMIT) {
    *dp = d;
    return true;
  }

  // Otherwise compute the correct integer using GetDecimal.
  return GetDecimal(start, end, dp);
}

namespace js {

template bool GetDecimalInteger(const char16_t* start, const char16_t* end,
                                double* dp);

template bool GetDecimalInteger(const Latin1Char* start, const Latin1Char* end,
                                double* dp);

template <>
bool GetDecimalInteger<Utf8Unit>(const Utf8Unit* start, const Utf8Unit* end,
                                 double* dp) {
  return GetDecimalInteger(Utf8AsUnsignedChars(start), Utf8AsUnsignedChars(end),
                           dp);
}

}  // namespace js

template <typename CharT>
bool js::GetDecimal(const CharT* start, const CharT* end, double* dp) {
  MOZ_ASSERT(start <= end);

  size_t length = end - start;

  auto convert = [](auto* chars, size_t length) -> double {
    using SToDConverter = double_conversion::StringToDoubleConverter;
    SToDConverter converter(/* flags = */ 0, /* empty_string_value = */ 0.0,
                            /* junk_string_value = */ 0.0,
                            /* infinity_symbol = */ nullptr,
                            /* nan_symbol = */ nullptr);
    int lengthInt = mozilla::AssertedCast<int>(length);
    int processed = 0;
    double d = converter.StringToDouble(chars, lengthInt, &processed);
    MOZ_ASSERT(processed >= 0);
    MOZ_ASSERT(size_t(processed) == length);
    return d;
  };

  // If there are no underscores, we don't need to copy the chars.
  bool hasUnderscore = std::any_of(start, end, [](auto c) { return c == '_'; });
  if (!hasUnderscore) {
    if constexpr (std::is_same_v<CharT, char16_t>) {
      *dp = convert(reinterpret_cast<const uc16*>(start), length);
    } else {
      static_assert(std::is_same_v<CharT, Latin1Char>);
      *dp = convert(reinterpret_cast<const char*>(start), length);
    }
    return true;
  }

  Vector<char, 32, SystemAllocPolicy> chars;
  if (!chars.growByUninitialized(length)) {
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

  *dp = convert(chars.begin(), i);
  return true;
}

namespace js {

template bool GetDecimal(const char16_t* start, const char16_t* end,
                         double* dp);

template bool GetDecimal(const Latin1Char* start, const Latin1Char* end,
                         double* dp);

template <>
bool GetDecimal<Utf8Unit>(const Utf8Unit* start, const Utf8Unit* end,
                          double* dp) {
  return GetDecimal(Utf8AsUnsignedChars(start), Utf8AsUnsignedChars(end), dp);
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
    d = js_strtod(begin, begin + linear->length(), &end);
    if (end == begin) {
      d = GenericNaN();
    }
  } else {
    const char16_t* begin = linear->twoByteChars(nogc);
    const char16_t* end;
    d = js_strtod(begin, begin + linear->length(), &end);
    if (end == begin) {
      d = GenericNaN();
    }
  }

  args.rval().setDouble(d);
  return true;
}

// ES2023 draft rev 053d34c87b14d9234d6f7f45bd61074b72ca9d69
// 19.2.5 parseInt ( string, radix )
template <typename CharT>
static bool ParseIntImpl(JSContext* cx, const CharT* chars, size_t length,
                         bool stripPrefix, int32_t radix, double* res) {
  // Step 2.
  const CharT* end = chars + length;
  const CharT* s = SkipSpace(chars, end);

  MOZ_ASSERT(chars <= s);
  MOZ_ASSERT(s <= end);

  // Steps 3-4.
  bool negative = (s != end && s[0] == '-');

  // Step 5. */
  if (s != end && (s[0] == '-' || s[0] == '+')) {
    s++;
  }

  // Step 10.
  if (stripPrefix) {
    if (end - s >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
      s += 2;
      radix = 16;
    }
  }

  // Steps 11-15.
  const CharT* actualEnd;
  double d;
  if (!js::GetPrefixInteger(s, end, radix, IntegerSeparatorHandling::None,
                            &actualEnd, &d)) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (s == actualEnd) {
    *res = GenericNaN();
  } else {
    *res = negative ? -d : d;
  }
  return true;
}

// ES2023 draft rev 053d34c87b14d9234d6f7f45bd61074b72ca9d69
// 19.2.5 parseInt ( string, radix )
bool js::NumberParseInt(JSContext* cx, HandleString str, int32_t radix,
                        MutableHandleValue result) {
  // Step 7.
  bool stripPrefix = true;

  // Steps 8-9.
  if (radix != 0) {
    if (radix < 2 || radix > 36) {
      result.setNaN();
      return true;
    }

    if (radix != 16) {
      stripPrefix = false;
    }
  } else {
    radix = 10;
  }
  MOZ_ASSERT(2 <= radix && radix <= 36);

  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  // Steps 2-5, 10-16.
  AutoCheckCannotGC nogc;
  size_t length = linear->length();
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

  result.setNumber(number);
  return true;
}

// ES2023 draft rev 053d34c87b14d9234d6f7f45bd61074b72ca9d69
// 19.2.5 parseInt ( string, radix )
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
     * To preserve this behaviour, we can't use the fast-path when string >=
     * 1e21, or else the result would be |NeM|.
     *
     * The same goes for values smaller than 1.0e-6, because the string would be
     * in the form of "Ne-M".
     */
    if (args[0].isDouble()) {
      double d = args[0].toDouble();
      if (DOUBLE_DECIMAL_IN_SHORTEST_LOW <= d &&
          d < DOUBLE_DECIMAL_IN_SHORTEST_HIGH) {
        args.rval().setNumber(floor(d));
        return true;
      }
      if (-DOUBLE_DECIMAL_IN_SHORTEST_HIGH < d &&
          d <= -DOUBLE_DECIMAL_IN_SHORTEST_LOW) {
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

  // Step 1.
  RootedString inputString(cx, ToString<CanGC>(cx, args[0]));
  if (!inputString) {
    return false;
  }

  // Step 6.
  int32_t radix = 0;
  if (args.hasDefined(1)) {
    if (!ToInt32(cx, args[1], &radix)) {
      return false;
    }
  }

  // Steps 2-5, 7-16.
  return NumberParseInt(cx, inputString, radix, args.rval());
}

static const JSFunctionSpec number_functions[] = {
    JS_SELF_HOSTED_FN("isNaN", "Global_isNaN", 1, JSPROP_RESOLVING),
    JS_SELF_HOSTED_FN("isFinite", "Global_isFinite", 1, JSPROP_RESOLVING),
    JS_FS_END};

const JSClass NumberObject::class_ = {
    "Number",
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
      !NumberValueToStringBuffer(NumberValue(d), sb) || !sb.append("))")) {
    return false;
  }

  JSString* str = sb.finishString();
  if (!str) {
    return false;
  }
  args.rval().setString(str);
  return true;
}

// Subtract one from DTOSTR_STANDARD_BUFFER_SIZE to exclude the null-character.
static_assert(
    double_conversion::DoubleToStringConverter::kMaxCharsEcmaScriptShortest ==
        DTOSTR_STANDARD_BUFFER_SIZE - 1,
    "double_conversion and dtoa both agree how large the longest string "
    "can be");

static_assert(DTOSTR_STANDARD_BUFFER_SIZE <= JS::MaximumNumberToStringLength,
              "MaximumNumberToStringLength is large enough to hold the longest "
              "string produced by a conversion");

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
  return js::Int32ToStringWithHeap<allowGC>(cx, si, gc::Heap::Default);
}
template JSLinearString* js::Int32ToString<CanGC>(JSContext* cx, int32_t si);
template JSLinearString* js::Int32ToString<NoGC>(JSContext* cx, int32_t si);

template <AllowGC allowGC>
JSLinearString* js::Int32ToStringWithHeap(JSContext* cx, int32_t si,
                                          gc::Heap heap) {
  if (JSLinearString* str = LookupInt32ToString(cx, si)) {
    return str;
  }

  Latin1Char buffer[JSFatInlineString::MAX_LENGTH_LATIN1 + 1];
  size_t length;
  Latin1Char* start =
      BackfillInt32InBuffer(si, buffer, std::size(buffer), &length);

  mozilla::Range<const Latin1Char> chars(start, length);
  JSInlineString* str = NewInlineString<allowGC>(cx, chars, heap);
  if (!str) {
    return nullptr;
  }
  if (si >= 0) {
    str->maybeInitializeIndexValue(si);
  }

  CacheNumber(cx, si, str);
  return str;
}
template JSLinearString* js::Int32ToStringWithHeap<CanGC>(JSContext* cx,
                                                          int32_t si,
                                                          gc::Heap heap);
template JSLinearString* js::Int32ToStringWithHeap<NoGC>(JSContext* cx,
                                                         int32_t si,
                                                         gc::Heap heap);

JSLinearString* js::Int32ToStringPure(JSContext* cx, int32_t si) {
  AutoUnsafeCallWithABI unsafe;
  return Int32ToString<NoGC>(cx, si);
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

  JSAtom* atom = Atomize(cx, start, length, indexValue);
  if (!atom) {
    return nullptr;
  }

  CacheNumber(cx, si, atom);
  return atom;
}

frontend::TaggedParserAtomIndex js::Int32ToParserAtom(
    FrontendContext* fc, frontend::ParserAtomsTable& parserAtoms, int32_t si) {
  char buffer[JSFatInlineString::MAX_LENGTH_TWO_BYTE + 1];
  size_t length;
  char* start = BackfillInt32InBuffer(
      si, buffer, JSFatInlineString::MAX_LENGTH_TWO_BYTE + 1, &length);

  Maybe<uint32_t> indexValue;
  if (si >= 0) {
    indexValue.emplace(si);
  }

  return parserAtoms.internAscii(fc, start, length);
}

/* Returns a non-nullptr pointer to inside `buf`. */
template <typename T>
static char* Int32ToCStringWithBase(mozilla::Range<char> buf, T i, size_t* len,
                                    int base) {
  uint32_t u;
  if constexpr (std::is_signed_v<T>) {
    u = Abs(i);
  } else {
    u = i;
  }

  RangedPtr<char> cp = buf.end() - 1;

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
  if constexpr (std::is_signed_v<T>) {
    if (i < 0) {
      *--cp = '-';
    }
  }

  *len = end - cp.get();
  return cp.get();
}

/* Returns a non-nullptr pointer to inside `out`. */
template <typename T, size_t Length>
static char* Int32ToCStringWithBase(char (&out)[Length], T i, size_t* len,
                                    int base) {
  // The buffer needs to be large enough to hold the largest number, including
  // the sign and the terminating null-character.
  static_assert(std::numeric_limits<T>::digits + (2 * std::is_signed_v<T>) <
                Length);

  mozilla::Range<char> buf(out, Length);
  return Int32ToCStringWithBase(buf, i, len, base);
}

/* Returns a non-nullptr pointer to inside `out`. */
template <typename T, size_t Base, size_t Length>
static char* Int32ToCString(char (&out)[Length], T i, size_t* len) {
  // The buffer needs to be large enough to hold the largest number, including
  // the sign and the terminating null-character.
  if constexpr (Base == 10) {
    static_assert(std::numeric_limits<T>::digits10 + 1 + std::is_signed_v<T> <
                  Length);
  } else {
    // Compute digits16 analog to std::numeric_limits::digits10, which is
    // defined as |std::numeric_limits::digits * std::log10(2)| for integer
    // types.
    // Note: log16(2) is 1/4.
    static_assert(Base == 16);
    static_assert(((std::numeric_limits<T>::digits + std::is_signed_v<T>) / 4 +
                   std::is_signed_v<T>) < Length);
  }

  mozilla::Range<char> buf(out, Length);
  return Int32ToCStringWithBase(buf, i, len, Base);
}

/* Returns a non-nullptr pointer to inside `cbuf`. */
template <typename T, size_t Base = 10>
static char* Int32ToCString(ToCStringBuf* cbuf, T i, size_t* len) {
  return Int32ToCString<T, Base>(cbuf->sbuf, i, len);
}

/* Returns a non-nullptr pointer to inside `cbuf`. */
template <typename T, size_t Base = 10>
static char* Int32ToCString(Int32ToCStringBuf* cbuf, T i, size_t* len) {
  return Int32ToCString<T, Base>(cbuf->sbuf, i, len);
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
    return false;
  }
  args.rval().setString(str);
  return true;
}

#if !JS_HAS_INTL_API
static bool num_toLocaleString(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Number.prototype",
                                        "toLocaleString");
  CallArgs args = CallArgsFromVp(argc, vp);

  double d;
  if (!ThisNumberValue(cx, args, "toLocaleString", &d)) {
    return false;
  }

  RootedString str(cx, NumberToStringWithBase<CanGC>(cx, d, 10));
  if (!str) {
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
  char* numStr = NumberToCString(&cbuf, prec);
  MOZ_ASSERT(numStr);
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_PRECISION_RANGE,
                            numStr);
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

  size_t numStrLen = builder.position();
  const char* numStr = builder.Finalize();
  MOZ_ASSERT(numStr == buf);
  MOZ_ASSERT(numStrLen == strlen(numStr));

  JSString* str = NewStringCopyN<CanGC>(cx, numStr, numStrLen);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

// ES 2021 draft 21.1.3.3.
static bool num_toFixed(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Number.prototype", "toFixed");
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
  if (std::isnan(d)) {
    args.rval().setString(cx->names().NaN);
    return true;
  }
  if (std::isinf(d)) {
    if (d > 0) {
      args.rval().setString(cx->names().Infinity);
      return true;
    }

    args.rval().setString(cx->names().NegativeInfinity_);
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
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Number.prototype",
                                        "toExponential");
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
  if (std::isnan(d)) {
    args.rval().setString(cx->names().NaN);
    return true;
  }
  if (std::isinf(d)) {
    if (d > 0) {
      args.rval().setString(cx->names().Infinity);
      return true;
    }

    args.rval().setString(cx->names().NegativeInfinity_);
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
  AutoJSMethodProfilerEntry pseudoFrame(cx, "Number.prototype", "toPrecision");
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
  if (std::isnan(d)) {
    args.rval().setString(cx->names().NaN);
    return true;
  }
  if (std::isinf(d)) {
    if (d > 0) {
      args.rval().setString(cx->names().Infinity);
      return true;
    }

    args.rval().setString(cx->names().NegativeInfinity_);
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
    JS_FN("toSource", num_toSource, 0, 0),
    JS_INLINABLE_FN("toString", num_toString, 1, 0, NumberToString),
#if JS_HAS_INTL_API
    JS_SELF_HOSTED_FN("toLocaleString", "Number_toLocaleString", 0, 0),
#else
    JS_FN("toLocaleString", num_toLocaleString, 0, 0),
#endif
    JS_FN("valueOf", num_valueOf, 0, 0),
    JS_FN("toFixed", num_toFixed, 1, 0),
    JS_FN("toExponential", num_toExponential, 1, 0),
    JS_FN("toPrecision", num_toPrecision, 1, 0),
    JS_FS_END};

bool js::IsInteger(double d) {
  return std::isfinite(d) && JS::ToInteger(d) == d;
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
  parseInt->setJitInfo(&jit::JitInfo_NumberParseInt);

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
    GenericCreateConstructor<Number, 1, gc::AllocKind::FUNCTION,
                             &jit::JitInfo_Number>,
    NumberObject::createPrototype,
    number_static_methods,
    number_static_properties,
    number_methods,
    nullptr,
    NumberClassFinish};

static char* FracNumberToCString(ToCStringBuf* cbuf, double d, size_t* len) {
#ifdef DEBUG
  {
    int32_t _;
    MOZ_ASSERT(!NumberEqualsInt32(d, &_));
  }
#endif

  /*
   * This is V8's implementation of the algorithm described in the
   * following paper:
   *
   *   Printing floating-point numbers quickly and accurately with integers.
   *   Florian Loitsch, PLDI 2010.
   */
  const double_conversion::DoubleToStringConverter& converter =
      double_conversion::DoubleToStringConverter::EcmaScriptConverter();
  double_conversion::StringBuilder builder(cbuf->sbuf, std::size(cbuf->sbuf));
  converter.ToShortest(d, &builder);

  *len = builder.position();
  return builder.Finalize();
}

void JS::NumberToString(double d, char (&out)[MaximumNumberToStringLength]) {
  int32_t i;
  if (NumberEqualsInt32(d, &i)) {
    Int32ToCStringBuf cbuf;
    size_t len;
    char* loc = ::Int32ToCString(&cbuf, i, &len);
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

char* js::NumberToCString(ToCStringBuf* cbuf, double d, size_t* length) {
  int32_t i;
  size_t len;
  char* s = NumberEqualsInt32(d, &i) ? ::Int32ToCString(cbuf, i, &len)
                                     : FracNumberToCString(cbuf, d, &len);
  MOZ_ASSERT(s);
  if (length) {
    *length = len;
  }
  return s;
}

char* js::Int32ToCString(Int32ToCStringBuf* cbuf, int32_t value,
                         size_t* length) {
  size_t len;
  char* s = ::Int32ToCString(cbuf, value, &len);
  MOZ_ASSERT(s);
  if (length) {
    *length = len;
  }
  return s;
}

char* js::Uint32ToCString(Int32ToCStringBuf* cbuf, uint32_t value,
                          size_t* length) {
  size_t len;
  char* s = ::Int32ToCString(cbuf, value, &len);
  MOZ_ASSERT(s);
  if (length) {
    *length = len;
  }
  return s;
}

char* js::Uint32ToHexCString(Int32ToCStringBuf* cbuf, uint32_t value,
                             size_t* length) {
  size_t len;
  char* s = ::Int32ToCString<uint32_t, 16>(cbuf, value, &len);
  MOZ_ASSERT(s);
  if (length) {
    *length = len;
  }
  return s;
}

template <AllowGC allowGC>
static JSString* NumberToStringWithBase(JSContext* cx, double d, int base) {
  MOZ_ASSERT(2 <= base && base <= 36);

  Realm* realm = cx->realm();

  int32_t i;
  if (NumberEqualsInt32(d, &i)) {
    bool isBase10Int = (base == 10);
    if (isBase10Int) {
      static_assert(StaticStrings::INT_STATIC_LIMIT > 10 * 10);
      if (StaticStrings::hasInt(i)) {
        return cx->staticStrings().getInt(i);
      }
    } else if (unsigned(i) < unsigned(base)) {
      if (i < 10) {
        return cx->staticStrings().getInt(i);
      }
      char16_t c = 'a' + i - 10;
      MOZ_ASSERT(StaticStrings::hasUnit(c));
      return cx->staticStrings().getUnit(c);
    } else if (unsigned(i) < unsigned(base * base)) {
      static constexpr char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
      char chars[] = {digits[i / base], digits[i % base]};
      JSString* str = cx->staticStrings().lookup(chars, 2);
      MOZ_ASSERT(str);
      return str;
    }

    if (JSLinearString* str = realm->dtoaCache.lookup(base, d)) {
      return str;
    }

    // Plus three to include the largest number, the sign, and the terminating
    // null character.
    constexpr size_t MaximumLength = std::numeric_limits<int32_t>::digits + 3;

    char buf[MaximumLength] = {};
    size_t numStrLen;
    char* numStr = Int32ToCStringWithBase(buf, i, &numStrLen, base);
    MOZ_ASSERT(numStrLen == strlen(numStr));

    JSLinearString* s = NewStringCopyN<allowGC>(cx, numStr, numStrLen);
    if (!s) {
      return nullptr;
    }

    if (isBase10Int && i >= 0) {
      s->maybeInitializeIndexValue(i);
    }

    realm->dtoaCache.cache(base, d, s);
    return s;
  }

  if (JSLinearString* str = realm->dtoaCache.lookup(base, d)) {
    return str;
  }

  JSLinearString* s;
  if (base == 10) {
    // We use a faster algorithm for base 10.
    ToCStringBuf cbuf;
    size_t numStrLen;
    char* numStr = FracNumberToCString(&cbuf, d, &numStrLen);
    MOZ_ASSERT(numStr);
    MOZ_ASSERT(numStrLen == strlen(numStr));

    s = NewStringCopyN<allowGC>(cx, numStr, numStrLen);
    if (!s) {
      return nullptr;
    }
  } else {
    if (!EnsureDtoaState(cx)) {
      if constexpr (allowGC) {
        ReportOutOfMemory(cx);
      }
      return nullptr;
    }

    UniqueChars numStr(js_dtobasestr(cx->dtoaState, base, d));
    if (!numStr) {
      if constexpr (allowGC) {
        ReportOutOfMemory(cx);
      }
      return nullptr;
    }

    s = NewStringCopyZ<allowGC>(cx, numStr.get());
    if (!s) {
      return nullptr;
    }
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
  return NumberToString<NoGC>(cx, d);
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
  size_t length;
  char* numStr = FracNumberToCString(&cbuf, d, &length);
  MOZ_ASSERT(numStr);
  MOZ_ASSERT(std::begin(cbuf.sbuf) <= numStr && numStr < std::end(cbuf.sbuf));
  MOZ_ASSERT(length == strlen(numStr));

  JSAtom* atom = Atomize(cx, numStr, length);
  if (!atom) {
    return nullptr;
  }

  CacheNumber(cx, d, atom);

  return atom;
}

frontend::TaggedParserAtomIndex js::NumberToParserAtom(
    FrontendContext* fc, frontend::ParserAtomsTable& parserAtoms, double d) {
  int32_t si;
  if (NumberEqualsInt32(d, &si)) {
    return Int32ToParserAtom(fc, parserAtoms, si);
  }

  ToCStringBuf cbuf;
  size_t length;
  char* numStr = FracNumberToCString(&cbuf, d, &length);
  MOZ_ASSERT(numStr);
  MOZ_ASSERT(std::begin(cbuf.sbuf) <= numStr && numStr < std::end(cbuf.sbuf));
  MOZ_ASSERT(length == strlen(numStr));

  return parserAtoms.internAscii(fc, numStr, length);
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
  JSInlineString* str =
      NewInlineString<CanGC>(cx, chars, js::gc::Heap::Default);
  if (!str) {
    return nullptr;
  }

  realm->dtoaCache.cache(10, index, str);
  return str;
}

JSString* js::Int32ToStringWithBase(JSContext* cx, int32_t i, int32_t base,
                                    bool lowerCase) {
  Rooted<JSString*> str(cx, NumberToStringWithBase<CanGC>(cx, double(i), base));
  if (!str) {
    return nullptr;
  }
  if (lowerCase) {
    return str;
  }
  return StringToUpperCase(cx, str);
}

bool js::NumberValueToStringBuffer(const Value& v, StringBuffer& sb) {
  /* Convert to C-string. */
  ToCStringBuf cbuf;
  const char* cstr;
  size_t cstrlen;
  if (v.isInt32()) {
    cstr = ::Int32ToCString(&cbuf, v.toInt32(), &cstrlen);
  } else {
    cstr = NumberToCString(&cbuf, v.toDouble(), &cstrlen);
  }
  MOZ_ASSERT(cstr);
  MOZ_ASSERT(cstrlen == strlen(cstr));

  MOZ_ASSERT(cstrlen < std::size(cbuf.sbuf));
  return sb.append(cstr, cstrlen);
}

template <typename CharT>
inline double CharToNumber(CharT c) {
  if ('0' <= c && c <= '9') {
    return c - '0';
  }
  if (unicode::IsSpace(c)) {
    return 0.0;
  }
  return GenericNaN();
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
  MOZ_ALWAYS_TRUE(GetPrefixIntegerImpl(
      start + 2, end, radix, IntegerSeparatorHandling::None, &endptr, &d));
  if (endptr == start + 2 || SkipSpace(endptr, end) != end) {
    *result = GenericNaN();
  } else {
    *result = d;
  }
  return true;
}

template <typename CharT>
double js::CharsToNumber(const CharT* chars, size_t length) {
  if (length == 1) {
    return CharToNumber(chars[0]);
  }

  const CharT* end = chars + length;
  const CharT* start = SkipSpace(chars, end);

  // ECMA doesn't allow signed non-decimal numbers (bug 273467).
  if (end - start >= 2 && start[0] == '0') {
    double d;
    if (CharsToNonDecimalNumber(start, end, &d)) {
      return d;
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
  double d = js_strtod(start, end, &ep);
  if (SkipSpace(ep, end) != end) {
    return GenericNaN();
  }
  return d;
}

template double js::CharsToNumber(const Latin1Char* chars, size_t length);

template double js::CharsToNumber(const char16_t* chars, size_t length);

double js::LinearStringToNumber(JSLinearString* str) {
  if (str->hasIndexValue()) {
    return str->getIndexValue();
  }

  AutoCheckCannotGC nogc;
  return str->hasLatin1Chars()
             ? CharsToNumber(str->latin1Chars(nogc), str->length())
             : CharsToNumber(str->twoByteChars(nogc), str->length());
}

bool js::StringToNumber(JSContext* cx, JSString* str, double* result) {
  JSLinearString* linearStr = str->ensureLinear(cx);
  if (!linearStr) {
    return false;
  }

  *result = LinearStringToNumber(linearStr);
  return true;
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

JS_PUBLIC_API bool js::ToNumberSlow(JSContext* cx, HandleValue v_,
                                    double* out) {
  RootedValue v(cx, v_);
  MOZ_ASSERT(!v.isNumber());

  if (!v.isPrimitive()) {
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
#ifdef ENABLE_RECORD_TUPLE
  if (v.isExtendedPrimitive()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_RECORD_TUPLE_TO_NUMBER);
    return false;
  }
#endif

  MOZ_ASSERT(v.isSymbol() || v.isBigInt());
  unsigned errnum = JSMSG_SYMBOL_TO_NUMBER;
  if (v.isBigInt()) {
    errnum = JSMSG_BIGINT_TO_NUMBER;
  }
  JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, errnum);
  return false;
}

// BigInt proposal section 3.1.6
bool js::ToNumericSlow(JSContext* cx, MutableHandleValue vp) {
  MOZ_ASSERT(!vp.isNumeric());

  // Step 1.
  if (!vp.isPrimitive()) {
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
double js_strtod(const CharT* begin, const CharT* end, const CharT** dEnd) {
  const CharT* s = SkipSpace(begin, end);
  size_t length = end - s;

  {
    // StringToDouble can make indirect calls but can't trigger a GC.
    JS::AutoSuppressGCAnalysis nogc;

    using SToDConverter = double_conversion::StringToDoubleConverter;
    SToDConverter converter(SToDConverter::ALLOW_TRAILING_JUNK,
                            /* empty_string_value = */ 0.0,
                            /* junk_string_value = */ GenericNaN(),
                            /* infinity_symbol = */ nullptr,
                            /* nan_symbol = */ nullptr);
    int lengthInt = mozilla::AssertedCast<int>(length);
    double d;
    int processed = 0;
    if constexpr (std::is_same_v<CharT, char16_t>) {
      d = converter.StringToDouble(reinterpret_cast<const uc16*>(s), lengthInt,
                                   &processed);
    } else {
      static_assert(std::is_same_v<CharT, Latin1Char>);
      d = converter.StringToDouble(reinterpret_cast<const char*>(s), lengthInt,
                                   &processed);
    }
    MOZ_ASSERT(processed >= 0);
    MOZ_ASSERT(processed <= lengthInt);

    if (processed > 0) {
      *dEnd = s + processed;
      return d;
    }
  }

  // Try to parse +Infinity, -Infinity or Infinity. Note that we do this here
  // instead of using StringToDoubleConverter's infinity_symbol because it's
  // faster: the code below is less generic and not on the fast path for regular
  // doubles.
  static constexpr std::string_view Infinity = "Infinity";
  if (length >= Infinity.length()) {
    const CharT* afterSign = s;
    bool negative = (*afterSign == '-');
    if (negative || *afterSign == '+') {
      afterSign++;
    }
    MOZ_ASSERT(afterSign < end);
    if (*afterSign == 'I' && size_t(end - afterSign) >= Infinity.length() &&
        EqualChars(afterSign, Infinity.data(), Infinity.length())) {
      *dEnd = afterSign + Infinity.length();
      return negative ? NegativeInfinity<double>() : PositiveInfinity<double>();
    }
  }

  *dEnd = begin;
  return 0.0;
}

template double js_strtod(const char16_t* begin, const char16_t* end,
                          const char16_t** dEnd);

template double js_strtod(const Latin1Char* begin, const Latin1Char* end,
                          const Latin1Char** dEnd);
