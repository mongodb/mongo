/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/String.h"

#include "mozilla/Attributes.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/FloatingPoint.h"
#if JS_HAS_INTL_API
#  include "mozilla/intl/String.h"
#endif
#include "mozilla/PodOperations.h"
#include "mozilla/Range.h"
#include "mozilla/SIMD.h"
#include "mozilla/TextUtils.h"

#include <algorithm>
#include <limits>
#include <string.h>
#include <type_traits>

#include "jsnum.h"
#include "jstypes.h"

#include "builtin/Array.h"
#if JS_HAS_INTL_API
#  include "builtin/intl/CommonFunctions.h"
#  include "builtin/intl/FormatBuffer.h"
#endif
#include "builtin/RegExp.h"
#include "jit/InlinableNatives.h"
#include "js/Conversions.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#if !JS_HAS_INTL_API
#  include "js/LocaleSensitive.h"
#endif
#include "js/Printer.h"
#include "js/PropertyAndElement.h"  // JS_DefineFunctions
#include "js/PropertySpec.h"
#include "js/StableStringChars.h"
#include "js/UniquePtr.h"
#include "util/StringBuffer.h"
#include "util/Unicode.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/RegExpObject.h"
#include "vm/SelfHosting.h"
#include "vm/StaticStrings.h"
#include "vm/ToSource.h"       // js::ValueToSource
#include "vm/WellKnownAtom.h"  // js_*_str

#include "vm/GeckoProfiler-inl.h"
#include "vm/InlineCharBuffer-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/StringObject-inl.h"
#include "vm/StringType-inl.h"

using namespace js;

using JS::Symbol;
using JS::SymbolCode;

using mozilla::AsciiAlphanumericToNumber;
using mozilla::CheckedInt;
using mozilla::EnsureUtf16ValiditySpan;
using mozilla::IsAsciiHexDigit;
using mozilla::PodCopy;
using mozilla::RangedPtr;
using mozilla::SIMD;
using mozilla::Span;
using mozilla::Utf16ValidUpTo;

using JS::AutoCheckCannotGC;
using JS::AutoStableStringChars;

static JSLinearString* ArgToLinearString(JSContext* cx, const CallArgs& args,
                                         unsigned argno) {
  if (argno >= args.length()) {
    return cx->names().undefined;
  }

  JSString* str = ToString<CanGC>(cx, args[argno]);
  if (!str) {
    return nullptr;
  }

  return str->ensureLinear(cx);
}

/*
 * Forward declarations for URI encode/decode and helper routines
 */
static bool str_decodeURI(JSContext* cx, unsigned argc, Value* vp);

static bool str_decodeURI_Component(JSContext* cx, unsigned argc, Value* vp);

static bool str_encodeURI(JSContext* cx, unsigned argc, Value* vp);

static bool str_encodeURI_Component(JSContext* cx, unsigned argc, Value* vp);

/*
 * Global string methods
 */

/* ES5 B.2.1 */
template <typename CharT>
static bool Escape(JSContext* cx, const CharT* chars, uint32_t length,
                   InlineCharBuffer<Latin1Char>& newChars,
                   uint32_t* newLengthOut) {
  // clang-format off
    static const uint8_t shouldPassThrough[128] = {
         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
         0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
         0,0,0,0,0,0,0,0,0,0,1,1,0,1,1,1,       /*    !"#$%&'()*+,-./  */
         1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,       /*   0123456789:;<=>?  */
         1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,       /*   @ABCDEFGHIJKLMNO  */
         1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,1,       /*   PQRSTUVWXYZ[\]^_  */
         0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,       /*   `abcdefghijklmno  */
         1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,0,       /*   pqrstuvwxyz{\}~  DEL */
    };
  // clang-format on

  /* Take a first pass and see how big the result string will need to be. */
  uint32_t newLength = length;
  for (size_t i = 0; i < length; i++) {
    char16_t ch = chars[i];
    if (ch < 128 && shouldPassThrough[ch]) {
      continue;
    }

    /*
     * newlength is incremented below by at most 5 and at this point it must
     * be a valid string length, so this should never overflow uint32_t.
     */
    static_assert(JSString::MAX_LENGTH < UINT32_MAX - 5,
                  "Adding 5 to valid string length should not overflow");

    MOZ_ASSERT(newLength <= JSString::MAX_LENGTH);

    /* The character will be encoded as %XX or %uXXXX. */
    newLength += (ch < 256) ? 2 : 5;

    if (MOZ_UNLIKELY(newLength > JSString::MAX_LENGTH)) {
      ReportAllocationOverflow(cx);
      return false;
    }
  }

  if (newLength == length) {
    *newLengthOut = newLength;
    return true;
  }

  if (!newChars.maybeAlloc(cx, newLength)) {
    return false;
  }

  static const char digits[] = "0123456789ABCDEF";

  Latin1Char* rawNewChars = newChars.get();
  size_t i, ni;
  for (i = 0, ni = 0; i < length; i++) {
    char16_t ch = chars[i];
    if (ch < 128 && shouldPassThrough[ch]) {
      rawNewChars[ni++] = ch;
    } else if (ch < 256) {
      rawNewChars[ni++] = '%';
      rawNewChars[ni++] = digits[ch >> 4];
      rawNewChars[ni++] = digits[ch & 0xF];
    } else {
      rawNewChars[ni++] = '%';
      rawNewChars[ni++] = 'u';
      rawNewChars[ni++] = digits[ch >> 12];
      rawNewChars[ni++] = digits[(ch & 0xF00) >> 8];
      rawNewChars[ni++] = digits[(ch & 0xF0) >> 4];
      rawNewChars[ni++] = digits[ch & 0xF];
    }
  }
  MOZ_ASSERT(ni == newLength);

  *newLengthOut = newLength;
  return true;
}

static bool str_escape(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "escape");
  CallArgs args = CallArgsFromVp(argc, vp);

  Rooted<JSLinearString*> str(cx, ArgToLinearString(cx, args, 0));
  if (!str) {
    return false;
  }

  InlineCharBuffer<Latin1Char> newChars;
  uint32_t newLength = 0;  // initialize to silence GCC warning
  if (str->hasLatin1Chars()) {
    AutoCheckCannotGC nogc;
    if (!Escape(cx, str->latin1Chars(nogc), str->length(), newChars,
                &newLength)) {
      return false;
    }
  } else {
    AutoCheckCannotGC nogc;
    if (!Escape(cx, str->twoByteChars(nogc), str->length(), newChars,
                &newLength)) {
      return false;
    }
  }

  // Return input if no characters need to be escaped.
  if (newLength == str->length()) {
    args.rval().setString(str);
    return true;
  }

  JSString* res = newChars.toString(cx, newLength);
  if (!res) {
    return false;
  }

  args.rval().setString(res);
  return true;
}

template <typename CharT>
static inline bool Unhex4(const RangedPtr<const CharT> chars,
                          char16_t* result) {
  CharT a = chars[0], b = chars[1], c = chars[2], d = chars[3];

  if (!(IsAsciiHexDigit(a) && IsAsciiHexDigit(b) && IsAsciiHexDigit(c) &&
        IsAsciiHexDigit(d))) {
    return false;
  }

  char16_t unhex = AsciiAlphanumericToNumber(a);
  unhex = (unhex << 4) + AsciiAlphanumericToNumber(b);
  unhex = (unhex << 4) + AsciiAlphanumericToNumber(c);
  unhex = (unhex << 4) + AsciiAlphanumericToNumber(d);
  *result = unhex;
  return true;
}

template <typename CharT>
static inline bool Unhex2(const RangedPtr<const CharT> chars,
                          char16_t* result) {
  CharT a = chars[0], b = chars[1];

  if (!(IsAsciiHexDigit(a) && IsAsciiHexDigit(b))) {
    return false;
  }

  *result = (AsciiAlphanumericToNumber(a) << 4) + AsciiAlphanumericToNumber(b);
  return true;
}

template <typename CharT>
static bool Unescape(StringBuffer& sb,
                     const mozilla::Range<const CharT> chars) {
  // Step 2.
  uint32_t length = chars.length();

  /*
   * Note that the spec algorithm has been optimized to avoid building
   * a string in the case where no escapes are present.
   */
  bool building = false;

#define ENSURE_BUILDING                            \
  do {                                             \
    if (!building) {                               \
      building = true;                             \
      if (!sb.reserve(length)) return false;       \
      sb.infallibleAppend(chars.begin().get(), k); \
    }                                              \
  } while (false);

  // Step 4.
  uint32_t k = 0;

  // Step 5.
  while (k < length) {
    // Step 5.a.
    char16_t c = chars[k];

    // Step 5.b.
    if (c == '%') {
      static_assert(JSString::MAX_LENGTH < UINT32_MAX - 6,
                    "String length is not near UINT32_MAX");

      // Steps 5.b.i-ii.
      if (k + 6 <= length && chars[k + 1] == 'u') {
        if (Unhex4(chars.begin() + k + 2, &c)) {
          ENSURE_BUILDING
          k += 5;
        }
      } else if (k + 3 <= length) {
        if (Unhex2(chars.begin() + k + 1, &c)) {
          ENSURE_BUILDING
          k += 2;
        }
      }
    }

    // Step 5.c.
    if (building && !sb.append(c)) {
      return false;
    }

    // Step 5.d.
    k += 1;
  }

  return true;
#undef ENSURE_BUILDING
}

// ES2018 draft rev f83aa38282c2a60c6916ebc410bfdf105a0f6a54
// B.2.1.2 unescape ( string )
static bool str_unescape(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "unescape");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  Rooted<JSLinearString*> str(cx, ArgToLinearString(cx, args, 0));
  if (!str) {
    return false;
  }

  // Step 3.
  JSStringBuilder sb(cx);
  if (str->hasTwoByteChars() && !sb.ensureTwoByteChars()) {
    return false;
  }

  // Steps 2, 4-5.
  bool unescapeFailed = false;
  if (str->hasLatin1Chars()) {
    AutoCheckCannotGC nogc;
    unescapeFailed = !Unescape(sb, str->latin1Range(nogc));
  } else {
    AutoCheckCannotGC nogc;
    unescapeFailed = !Unescape(sb, str->twoByteRange(nogc));
  }
  if (unescapeFailed) {
    return false;
  }

  // Step 6.
  JSLinearString* result;
  if (!sb.empty()) {
    result = sb.finishString();
    if (!result) {
      return false;
    }
  } else {
    result = str;
  }

  args.rval().setString(result);
  return true;
}

static bool str_uneval(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  JSString* str = ValueToSource(cx, args.get(0));
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

static const JSFunctionSpec string_functions[] = {
    JS_FN(js_escape_str, str_escape, 1, JSPROP_RESOLVING),
    JS_FN(js_unescape_str, str_unescape, 1, JSPROP_RESOLVING),
    JS_FN(js_uneval_str, str_uneval, 1, JSPROP_RESOLVING),
    JS_FN(js_decodeURI_str, str_decodeURI, 1, JSPROP_RESOLVING),
    JS_FN(js_encodeURI_str, str_encodeURI, 1, JSPROP_RESOLVING),
    JS_FN(js_decodeURIComponent_str, str_decodeURI_Component, 1,
          JSPROP_RESOLVING),
    JS_FN(js_encodeURIComponent_str, str_encodeURI_Component, 1,
          JSPROP_RESOLVING),

    JS_FS_END};

static const unsigned STRING_ELEMENT_ATTRS =
    JSPROP_ENUMERATE | JSPROP_READONLY | JSPROP_PERMANENT;

static bool str_enumerate(JSContext* cx, HandleObject obj) {
  RootedString str(cx, obj->as<StringObject>().unbox());
  js::StaticStrings& staticStrings = cx->staticStrings();

  RootedValue value(cx);
  for (size_t i = 0, length = str->length(); i < length; i++) {
    JSString* str1 = staticStrings.getUnitStringForElement(cx, str, i);
    if (!str1) {
      return false;
    }
    value.setString(str1);
    if (!DefineDataElement(cx, obj, i, value,
                           STRING_ELEMENT_ATTRS | JSPROP_RESOLVING)) {
      return false;
    }
  }

  return true;
}

static bool str_mayResolve(const JSAtomState&, jsid id, JSObject*) {
  // str_resolve ignores non-integer ids.
  return id.isInt();
}

static bool str_resolve(JSContext* cx, HandleObject obj, HandleId id,
                        bool* resolvedp) {
  if (!id.isInt()) {
    return true;
  }

  RootedString str(cx, obj->as<StringObject>().unbox());

  int32_t slot = id.toInt();
  if ((size_t)slot < str->length()) {
    JSString* str1 =
        cx->staticStrings().getUnitStringForElement(cx, str, size_t(slot));
    if (!str1) {
      return false;
    }
    RootedValue value(cx, StringValue(str1));
    if (!DefineDataElement(cx, obj, uint32_t(slot), value,
                           STRING_ELEMENT_ATTRS | JSPROP_RESOLVING)) {
      return false;
    }
    *resolvedp = true;
  }
  return true;
}

static const JSClassOps StringObjectClassOps = {
    nullptr,         // addProperty
    nullptr,         // delProperty
    str_enumerate,   // enumerate
    nullptr,         // newEnumerate
    str_resolve,     // resolve
    str_mayResolve,  // mayResolve
    nullptr,         // finalize
    nullptr,         // call
    nullptr,         // construct
    nullptr,         // trace
};

const JSClass StringObject::class_ = {
    js_String_str,
    JSCLASS_HAS_RESERVED_SLOTS(StringObject::RESERVED_SLOTS) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_String),
    &StringObjectClassOps, &StringObject::classSpec_};

/*
 * Perform the initial |RequireObjectCoercible(thisv)| and |ToString(thisv)|
 * from nearly all String.prototype.* functions.
 */
static MOZ_ALWAYS_INLINE JSString* ToStringForStringFunction(
    JSContext* cx, const char* funName, HandleValue thisv) {
  if (thisv.isString()) {
    return thisv.toString();
  }

  if (thisv.isObject()) {
    if (thisv.toObject().is<StringObject>()) {
      StringObject* nobj = &thisv.toObject().as<StringObject>();
      // We have to make sure that the ToPrimitive call from ToString
      // would be unobservable.
      if (HasNoToPrimitiveMethodPure(nobj, cx) &&
          HasNativeMethodPure(nobj, cx->names().toString, str_toString, cx)) {
        return nobj->unbox();
      }
    }
  } else if (thisv.isNullOrUndefined()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INCOMPATIBLE_PROTO, "String", funName,
                              thisv.isNull() ? "null" : "undefined");
    return nullptr;
  }

  return ToStringSlow<CanGC>(cx, thisv);
}

MOZ_ALWAYS_INLINE bool IsString(HandleValue v) {
  return v.isString() || (v.isObject() && v.toObject().is<StringObject>());
}

MOZ_ALWAYS_INLINE bool str_toSource_impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsString(args.thisv()));

  JSString* str = ToString<CanGC>(cx, args.thisv());
  if (!str) {
    return false;
  }

  UniqueChars quoted = QuoteString(cx, str, '"');
  if (!quoted) {
    return false;
  }

  JSStringBuilder sb(cx);
  if (!sb.append("(new String(") ||
      !sb.append(quoted.get(), strlen(quoted.get())) || !sb.append("))")) {
    return false;
  }

  JSString* result = sb.finishString();
  if (!result) {
    return false;
  }
  args.rval().setString(result);
  return true;
}

static bool str_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsString, str_toSource_impl>(cx, args);
}

MOZ_ALWAYS_INLINE bool str_toString_impl(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsString(args.thisv()));

  args.rval().setString(
      args.thisv().isString()
          ? args.thisv().toString()
          : args.thisv().toObject().as<StringObject>().unbox());
  return true;
}

bool js::str_toString(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsString, str_toString_impl>(cx, args);
}

/*
 * Java-like string native methods.
 */

JSString* js::SubstringKernel(JSContext* cx, HandleString str, int32_t beginInt,
                              int32_t lengthInt) {
  MOZ_ASSERT(0 <= beginInt);
  MOZ_ASSERT(0 <= lengthInt);
  MOZ_ASSERT(uint32_t(beginInt) <= str->length());
  MOZ_ASSERT(uint32_t(lengthInt) <= str->length() - beginInt);

  uint32_t begin = beginInt;
  uint32_t len = lengthInt;

  /*
   * Optimization for one level deep ropes.
   * This is common for the following pattern:
   *
   * while() {
   *   text = text.substr(0, x) + "bla" + text.substr(x)
   *   test.charCodeAt(x + 1)
   * }
   */
  if (str->isRope()) {
    JSRope* rope = &str->asRope();

    /* Substring is totally in leftChild of rope. */
    if (begin + len <= rope->leftChild()->length()) {
      return NewDependentString(cx, rope->leftChild(), begin, len);
    }

    /* Substring is totally in rightChild of rope. */
    if (begin >= rope->leftChild()->length()) {
      begin -= rope->leftChild()->length();
      return NewDependentString(cx, rope->rightChild(), begin, len);
    }

    /*
     * Requested substring is partly in the left and partly in right child.
     * Create a rope of substrings for both childs.
     */
    MOZ_ASSERT(begin < rope->leftChild()->length() &&
               begin + len > rope->leftChild()->length());

    size_t lhsLength = rope->leftChild()->length() - begin;
    size_t rhsLength = begin + len - rope->leftChild()->length();

    Rooted<JSRope*> ropeRoot(cx, rope);
    RootedString lhs(
        cx, NewDependentString(cx, ropeRoot->leftChild(), begin, lhsLength));
    if (!lhs) {
      return nullptr;
    }

    RootedString rhs(
        cx, NewDependentString(cx, ropeRoot->rightChild(), 0, rhsLength));
    if (!rhs) {
      return nullptr;
    }

    return JSRope::new_<CanGC>(cx, lhs, rhs, len);
  }

  return NewDependentString(cx, str, begin, len);
}

/**
 * U+03A3 GREEK CAPITAL LETTER SIGMA has two different lower case mappings
 * depending on its context:
 * When it's preceded by a cased character and not followed by another cased
 * character, its lower case form is U+03C2 GREEK SMALL LETTER FINAL SIGMA.
 * Otherwise its lower case mapping is U+03C3 GREEK SMALL LETTER SIGMA.
 *
 * Unicode 9.0, §3.13 Default Case Algorithms
 */
static char16_t Final_Sigma(const char16_t* chars, size_t length,
                            size_t index) {
  MOZ_ASSERT(index < length);
  MOZ_ASSERT(chars[index] == unicode::GREEK_CAPITAL_LETTER_SIGMA);
  MOZ_ASSERT(unicode::ToLowerCase(unicode::GREEK_CAPITAL_LETTER_SIGMA) ==
             unicode::GREEK_SMALL_LETTER_SIGMA);

#if JS_HAS_INTL_API
  // Tell the analysis the BinaryProperty.contains function pointer called by
  // mozilla::intl::String::Is{CaseIgnorable, Cased} cannot GC.
  JS::AutoSuppressGCAnalysis nogc;

  bool precededByCased = false;
  for (size_t i = index; i > 0;) {
    char16_t c = chars[--i];
    char32_t codePoint = c;
    if (unicode::IsTrailSurrogate(c) && i > 0) {
      char16_t lead = chars[i - 1];
      if (unicode::IsLeadSurrogate(lead)) {
        codePoint = unicode::UTF16Decode(lead, c);
        i--;
      }
    }

    // Ignore any characters with the property Case_Ignorable.
    // NB: We need to skip over all Case_Ignorable characters, even when
    // they also have the Cased binary property.
    if (mozilla::intl::String::IsCaseIgnorable(codePoint)) {
      continue;
    }

    precededByCased = mozilla::intl::String::IsCased(codePoint);
    break;
  }
  if (!precededByCased) {
    return unicode::GREEK_SMALL_LETTER_SIGMA;
  }

  bool followedByCased = false;
  for (size_t i = index + 1; i < length;) {
    char16_t c = chars[i++];
    char32_t codePoint = c;
    if (unicode::IsLeadSurrogate(c) && i < length) {
      char16_t trail = chars[i];
      if (unicode::IsTrailSurrogate(trail)) {
        codePoint = unicode::UTF16Decode(c, trail);
        i++;
      }
    }

    // Ignore any characters with the property Case_Ignorable.
    // NB: We need to skip over all Case_Ignorable characters, even when
    // they also have the Cased binary property.
    if (mozilla::intl::String::IsCaseIgnorable(codePoint)) {
      continue;
    }

    followedByCased = mozilla::intl::String::IsCased(codePoint);
    break;
  }
  if (!followedByCased) {
    return unicode::GREEK_SMALL_LETTER_FINAL_SIGMA;
  }
#endif

  return unicode::GREEK_SMALL_LETTER_SIGMA;
}

// If |srcLength == destLength| is true, the destination buffer was allocated
// with the same size as the source buffer. When we append characters which
// have special casing mappings, we test |srcLength == destLength| to decide
// if we need to back out and reallocate a sufficiently large destination
// buffer. Otherwise the destination buffer was allocated with the correct
// size to hold all lower case mapped characters, i.e.
// |destLength == ToLowerCaseLength(srcChars, 0, srcLength)| is true.
template <typename CharT>
static size_t ToLowerCaseImpl(CharT* destChars, const CharT* srcChars,
                              size_t startIndex, size_t srcLength,
                              size_t destLength) {
  MOZ_ASSERT(startIndex < srcLength);
  MOZ_ASSERT(srcLength <= destLength);
  if constexpr (std::is_same_v<CharT, Latin1Char>) {
    MOZ_ASSERT(srcLength == destLength);
  }

  size_t j = startIndex;
  for (size_t i = startIndex; i < srcLength; i++) {
    CharT c = srcChars[i];
    if constexpr (!std::is_same_v<CharT, Latin1Char>) {
      if (unicode::IsLeadSurrogate(c) && i + 1 < srcLength) {
        char16_t trail = srcChars[i + 1];
        if (unicode::IsTrailSurrogate(trail)) {
          trail = unicode::ToLowerCaseNonBMPTrail(c, trail);
          destChars[j++] = c;
          destChars[j++] = trail;
          i++;
          continue;
        }
      }

      // Special case: U+0130 LATIN CAPITAL LETTER I WITH DOT ABOVE
      // lowercases to <U+0069 U+0307>.
      if (c == unicode::LATIN_CAPITAL_LETTER_I_WITH_DOT_ABOVE) {
        // Return if the output buffer is too small.
        if (srcLength == destLength) {
          return i;
        }

        destChars[j++] = CharT('i');
        destChars[j++] = CharT(unicode::COMBINING_DOT_ABOVE);
        continue;
      }

      // Special case: U+03A3 GREEK CAPITAL LETTER SIGMA lowercases to
      // one of two codepoints depending on context.
      if (c == unicode::GREEK_CAPITAL_LETTER_SIGMA) {
        destChars[j++] = Final_Sigma(srcChars, srcLength, i);
        continue;
      }
    }

    c = unicode::ToLowerCase(c);
    destChars[j++] = c;
  }

  MOZ_ASSERT(j == destLength);
  return srcLength;
}

static size_t ToLowerCaseLength(const char16_t* chars, size_t startIndex,
                                size_t length) {
  size_t lowerLength = length;
  for (size_t i = startIndex; i < length; i++) {
    char16_t c = chars[i];

    // U+0130 is lowercased to the two-element sequence <U+0069 U+0307>.
    if (c == unicode::LATIN_CAPITAL_LETTER_I_WITH_DOT_ABOVE) {
      lowerLength += 1;
    }
  }
  return lowerLength;
}

template <typename CharT>
static JSString* ToLowerCase(JSContext* cx, JSLinearString* str) {
  // Unlike toUpperCase, toLowerCase has the nice invariant that if the
  // input is a Latin-1 string, the output is also a Latin-1 string.

  InlineCharBuffer<CharT> newChars;

  const size_t length = str->length();
  size_t resultLength;
  {
    AutoCheckCannotGC nogc;
    const CharT* chars = str->chars<CharT>(nogc);

    // We don't need extra special casing checks in the loop below,
    // because U+0130 LATIN CAPITAL LETTER I WITH DOT ABOVE and U+03A3
    // GREEK CAPITAL LETTER SIGMA already have simple lower case mappings.
    MOZ_ASSERT(unicode::ChangesWhenLowerCased(
                   unicode::LATIN_CAPITAL_LETTER_I_WITH_DOT_ABOVE),
               "U+0130 has a simple lower case mapping");
    MOZ_ASSERT(
        unicode::ChangesWhenLowerCased(unicode::GREEK_CAPITAL_LETTER_SIGMA),
        "U+03A3 has a simple lower case mapping");

    // One element Latin-1 strings can be directly retrieved from the
    // static strings cache.
    if constexpr (std::is_same_v<CharT, Latin1Char>) {
      if (length == 1) {
        CharT lower = unicode::ToLowerCase(chars[0]);
        MOZ_ASSERT(StaticStrings::hasUnit(lower));

        return cx->staticStrings().getUnit(lower);
      }
    }

    // Look for the first character that changes when lowercased.
    size_t i = 0;
    for (; i < length; i++) {
      CharT c = chars[i];
      if constexpr (!std::is_same_v<CharT, Latin1Char>) {
        if (unicode::IsLeadSurrogate(c) && i + 1 < length) {
          CharT trail = chars[i + 1];
          if (unicode::IsTrailSurrogate(trail)) {
            if (unicode::ChangesWhenLowerCasedNonBMP(c, trail)) {
              break;
            }

            i++;
            continue;
          }
        }
      }
      if (unicode::ChangesWhenLowerCased(c)) {
        break;
      }
    }

    // If no character needs to change, return the input string.
    if (i == length) {
      return str;
    }

    resultLength = length;
    if (!newChars.maybeAlloc(cx, resultLength)) {
      return nullptr;
    }

    PodCopy(newChars.get(), chars, i);

    size_t readChars =
        ToLowerCaseImpl(newChars.get(), chars, i, length, resultLength);
    if constexpr (!std::is_same_v<CharT, Latin1Char>) {
      if (readChars < length) {
        resultLength = ToLowerCaseLength(chars, readChars, length);

        if (!newChars.maybeRealloc(cx, length, resultLength)) {
          return nullptr;
        }

        MOZ_ALWAYS_TRUE(length == ToLowerCaseImpl(newChars.get(), chars,
                                                  readChars, length,
                                                  resultLength));
      }
    } else {
      MOZ_ASSERT(readChars == length,
                 "Latin-1 strings don't have special lower case mappings");
    }
  }

  return newChars.toStringDontDeflate(cx, resultLength);
}

JSString* js::StringToLowerCase(JSContext* cx, HandleString string) {
  JSLinearString* linear = string->ensureLinear(cx);
  if (!linear) {
    return nullptr;
  }

  if (linear->hasLatin1Chars()) {
    return ToLowerCase<Latin1Char>(cx, linear);
  }
  return ToLowerCase<char16_t>(cx, linear);
}

static bool str_toLowerCase(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "toLowerCase");
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedString str(cx,
                   ToStringForStringFunction(cx, "toLowerCase", args.thisv()));
  if (!str) {
    return false;
  }

  JSString* result = StringToLowerCase(cx, str);
  if (!result) {
    return false;
  }

  args.rval().setString(result);
  return true;
}

#if JS_HAS_INTL_API
// String.prototype.toLocaleLowerCase is self-hosted when Intl is exposed,
// with core functionality performed by the intrinsic below.

static const char* CaseMappingLocale(JSContext* cx, JSString* str) {
  JSLinearString* locale = str->ensureLinear(cx);
  if (!locale) {
    return nullptr;
  }

  MOZ_ASSERT(locale->length() >= 2, "locale is a valid language tag");

  // Lithuanian, Turkish, and Azeri have language dependent case mappings.
  static const char languagesWithSpecialCasing[][3] = {"lt", "tr", "az"};

  // All strings in |languagesWithSpecialCasing| are of length two, so we
  // only need to compare the first two characters to find a matching locale.
  // ES2017 Intl, §9.2.2 BestAvailableLocale
  if (locale->length() == 2 || locale->latin1OrTwoByteChar(2) == '-') {
    for (const auto& language : languagesWithSpecialCasing) {
      if (locale->latin1OrTwoByteChar(0) == language[0] &&
          locale->latin1OrTwoByteChar(1) == language[1]) {
        return language;
      }
    }
  }

  return "";  // ICU root locale
}

static bool HasDefaultCasing(const char* locale) { return !strcmp(locale, ""); }

bool js::intl_toLocaleLowerCase(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(args[0].isString());
  MOZ_ASSERT(args[1].isString());

  RootedString string(cx, args[0].toString());

  const char* locale = CaseMappingLocale(cx, args[1].toString());
  if (!locale) {
    return false;
  }

  // Call String.prototype.toLowerCase() for language independent casing.
  if (HasDefaultCasing(locale)) {
    JSString* str = StringToLowerCase(cx, string);
    if (!str) {
      return false;
    }

    args.rval().setString(str);
    return true;
  }

  AutoStableStringChars inputChars(cx);
  if (!inputChars.initTwoByte(cx, string)) {
    return false;
  }
  mozilla::Range<const char16_t> input = inputChars.twoByteRange();

  // Note: maximum case mapping length is three characters, so the result
  // length might be > INT32_MAX. ICU will fail in this case.
  static_assert(JSString::MAX_LENGTH <= INT32_MAX,
                "String length must fit in int32_t for ICU");

  static const size_t INLINE_CAPACITY = js::intl::INITIAL_CHAR_BUFFER_SIZE;

  intl::FormatBuffer<char16_t, INLINE_CAPACITY> buffer(cx);

  auto ok = mozilla::intl::String::ToLocaleLowerCase(locale, input, buffer);
  if (ok.isErr()) {
    intl::ReportInternalError(cx, ok.unwrapErr());
    return false;
  }

  JSString* result = buffer.toString(cx);
  if (!result) {
    return false;
  }

  args.rval().setString(result);
  return true;
}

#else

// When the Intl API is not exposed, String.prototype.toLowerCase is implemented
// in C++.
static bool str_toLocaleLowerCase(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype",
                                        "toLocaleLowerCase");
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedString str(
      cx, ToStringForStringFunction(cx, "toLocaleLowerCase", args.thisv()));
  if (!str) {
    return false;
  }

  /*
   * Forcefully ignore the first (or any) argument and return toLowerCase(),
   * ECMA has reserved that argument, presumably for defining the locale.
   */
  if (cx->runtime()->localeCallbacks &&
      cx->runtime()->localeCallbacks->localeToLowerCase) {
    RootedValue result(cx);
    if (!cx->runtime()->localeCallbacks->localeToLowerCase(cx, str, &result)) {
      return false;
    }

    args.rval().set(result);
    return true;
  }

  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  JSString* result = StringToLowerCase(cx, linear);
  if (!result) {
    return false;
  }

  args.rval().setString(result);
  return true;
}

#endif  // JS_HAS_INTL_API

static inline bool ToUpperCaseHasSpecialCasing(Latin1Char charCode) {
  // U+00DF LATIN SMALL LETTER SHARP S is the only Latin-1 code point with
  // special casing rules, so detect it inline.
  bool hasUpperCaseSpecialCasing =
      charCode == unicode::LATIN_SMALL_LETTER_SHARP_S;
  MOZ_ASSERT(hasUpperCaseSpecialCasing ==
             unicode::ChangesWhenUpperCasedSpecialCasing(charCode));

  return hasUpperCaseSpecialCasing;
}

static inline bool ToUpperCaseHasSpecialCasing(char16_t charCode) {
  return unicode::ChangesWhenUpperCasedSpecialCasing(charCode);
}

static inline size_t ToUpperCaseLengthSpecialCasing(Latin1Char charCode) {
  // U+00DF LATIN SMALL LETTER SHARP S is uppercased to two 'S'.
  MOZ_ASSERT(charCode == unicode::LATIN_SMALL_LETTER_SHARP_S);

  return 2;
}

static inline size_t ToUpperCaseLengthSpecialCasing(char16_t charCode) {
  MOZ_ASSERT(ToUpperCaseHasSpecialCasing(charCode));

  return unicode::LengthUpperCaseSpecialCasing(charCode);
}

static inline void ToUpperCaseAppendUpperCaseSpecialCasing(char16_t charCode,
                                                           Latin1Char* elements,
                                                           size_t* index) {
  // U+00DF LATIN SMALL LETTER SHARP S is uppercased to two 'S'.
  MOZ_ASSERT(charCode == unicode::LATIN_SMALL_LETTER_SHARP_S);
  static_assert('S' <= JSString::MAX_LATIN1_CHAR, "'S' is a Latin-1 character");

  elements[(*index)++] = 'S';
  elements[(*index)++] = 'S';
}

static inline void ToUpperCaseAppendUpperCaseSpecialCasing(char16_t charCode,
                                                           char16_t* elements,
                                                           size_t* index) {
  unicode::AppendUpperCaseSpecialCasing(charCode, elements, index);
}

// See ToLowerCaseImpl for an explanation of the parameters.
template <typename DestChar, typename SrcChar>
static size_t ToUpperCaseImpl(DestChar* destChars, const SrcChar* srcChars,
                              size_t startIndex, size_t srcLength,
                              size_t destLength) {
  static_assert(std::is_same_v<SrcChar, Latin1Char> ||
                    !std::is_same_v<DestChar, Latin1Char>,
                "cannot write non-Latin-1 characters into Latin-1 string");
  MOZ_ASSERT(startIndex < srcLength);
  MOZ_ASSERT(srcLength <= destLength);

  size_t j = startIndex;
  for (size_t i = startIndex; i < srcLength; i++) {
    char16_t c = srcChars[i];
    if constexpr (!std::is_same_v<DestChar, Latin1Char>) {
      if (unicode::IsLeadSurrogate(c) && i + 1 < srcLength) {
        char16_t trail = srcChars[i + 1];
        if (unicode::IsTrailSurrogate(trail)) {
          trail = unicode::ToUpperCaseNonBMPTrail(c, trail);
          destChars[j++] = c;
          destChars[j++] = trail;
          i++;
          continue;
        }
      }
    }

    if (MOZ_UNLIKELY(c > 0x7f &&
                     ToUpperCaseHasSpecialCasing(static_cast<SrcChar>(c)))) {
      // Return if the output buffer is too small.
      if (srcLength == destLength) {
        return i;
      }

      ToUpperCaseAppendUpperCaseSpecialCasing(c, destChars, &j);
      continue;
    }

    c = unicode::ToUpperCase(c);
    if constexpr (std::is_same_v<DestChar, Latin1Char>) {
      MOZ_ASSERT(c <= JSString::MAX_LATIN1_CHAR);
    }
    destChars[j++] = c;
  }

  MOZ_ASSERT(j == destLength);
  return srcLength;
}

template <typename CharT>
static size_t ToUpperCaseLength(const CharT* chars, size_t startIndex,
                                size_t length) {
  size_t upperLength = length;
  for (size_t i = startIndex; i < length; i++) {
    char16_t c = chars[i];

    if (c > 0x7f && ToUpperCaseHasSpecialCasing(static_cast<CharT>(c))) {
      upperLength += ToUpperCaseLengthSpecialCasing(static_cast<CharT>(c)) - 1;
    }
  }
  return upperLength;
}

template <typename DestChar, typename SrcChar>
static inline void CopyChars(DestChar* destChars, const SrcChar* srcChars,
                             size_t length) {
  static_assert(!std::is_same_v<DestChar, SrcChar>,
                "PodCopy is used for the same type case");
  for (size_t i = 0; i < length; i++) {
    destChars[i] = srcChars[i];
  }
}

template <typename CharT>
static inline void CopyChars(CharT* destChars, const CharT* srcChars,
                             size_t length) {
  PodCopy(destChars, srcChars, length);
}

template <typename DestChar, typename SrcChar>
static inline bool ToUpperCase(JSContext* cx,
                               InlineCharBuffer<DestChar>& newChars,
                               const SrcChar* chars, size_t startIndex,
                               size_t length, size_t* resultLength) {
  MOZ_ASSERT(startIndex < length);

  *resultLength = length;
  if (!newChars.maybeAlloc(cx, length)) {
    return false;
  }

  CopyChars(newChars.get(), chars, startIndex);

  size_t readChars =
      ToUpperCaseImpl(newChars.get(), chars, startIndex, length, length);
  if (readChars < length) {
    size_t actualLength = ToUpperCaseLength(chars, readChars, length);

    *resultLength = actualLength;
    if (!newChars.maybeRealloc(cx, length, actualLength)) {
      return false;
    }

    MOZ_ALWAYS_TRUE(length == ToUpperCaseImpl(newChars.get(), chars, readChars,
                                              length, actualLength));
  }

  return true;
}

template <typename CharT>
static JSString* ToUpperCase(JSContext* cx, JSLinearString* str) {
  using Latin1Buffer = InlineCharBuffer<Latin1Char>;
  using TwoByteBuffer = InlineCharBuffer<char16_t>;

  mozilla::MaybeOneOf<Latin1Buffer, TwoByteBuffer> newChars;
  const size_t length = str->length();
  size_t resultLength;
  {
    AutoCheckCannotGC nogc;
    const CharT* chars = str->chars<CharT>(nogc);

    // Most one element Latin-1 strings can be directly retrieved from the
    // static strings cache.
    if constexpr (std::is_same_v<CharT, Latin1Char>) {
      if (length == 1) {
        Latin1Char c = chars[0];
        if (c != unicode::MICRO_SIGN &&
            c != unicode::LATIN_SMALL_LETTER_Y_WITH_DIAERESIS &&
            c != unicode::LATIN_SMALL_LETTER_SHARP_S) {
          char16_t upper = unicode::ToUpperCase(c);
          MOZ_ASSERT(upper <= JSString::MAX_LATIN1_CHAR);
          MOZ_ASSERT(StaticStrings::hasUnit(upper));

          return cx->staticStrings().getUnit(upper);
        }

        MOZ_ASSERT(unicode::ToUpperCase(c) > JSString::MAX_LATIN1_CHAR ||
                   ToUpperCaseHasSpecialCasing(c));
      }
    }

    // Look for the first character that changes when uppercased.
    size_t i = 0;
    for (; i < length; i++) {
      CharT c = chars[i];
      if constexpr (!std::is_same_v<CharT, Latin1Char>) {
        if (unicode::IsLeadSurrogate(c) && i + 1 < length) {
          CharT trail = chars[i + 1];
          if (unicode::IsTrailSurrogate(trail)) {
            if (unicode::ChangesWhenUpperCasedNonBMP(c, trail)) {
              break;
            }

            i++;
            continue;
          }
        }
      }
      if (unicode::ChangesWhenUpperCased(c)) {
        break;
      }
      if (MOZ_UNLIKELY(c > 0x7f && ToUpperCaseHasSpecialCasing(c))) {
        break;
      }
    }

    // If no character needs to change, return the input string.
    if (i == length) {
      return str;
    }

    // The string changes when uppercased, so we must create a new string.
    // Can it be Latin-1?
    //
    // If the original string is Latin-1, it can -- unless the string
    // contains U+00B5 MICRO SIGN or U+00FF SMALL LETTER Y WITH DIAERESIS,
    // the only Latin-1 codepoints that don't uppercase within Latin-1.
    // Search for those codepoints to decide whether the new string can be
    // Latin-1.
    // If the original string is a two-byte string, its uppercase form is
    // so rarely Latin-1 that we don't even consider creating a new
    // Latin-1 string.
    if constexpr (std::is_same_v<CharT, Latin1Char>) {
      bool resultIsLatin1 = true;
      for (size_t j = i; j < length; j++) {
        Latin1Char c = chars[j];
        if (c == unicode::MICRO_SIGN ||
            c == unicode::LATIN_SMALL_LETTER_Y_WITH_DIAERESIS) {
          MOZ_ASSERT(unicode::ToUpperCase(c) > JSString::MAX_LATIN1_CHAR);
          resultIsLatin1 = false;
          break;
        } else {
          MOZ_ASSERT(unicode::ToUpperCase(c) <= JSString::MAX_LATIN1_CHAR);
        }
      }

      if (resultIsLatin1) {
        newChars.construct<Latin1Buffer>();

        if (!ToUpperCase(cx, newChars.ref<Latin1Buffer>(), chars, i, length,
                         &resultLength)) {
          return nullptr;
        }
      } else {
        newChars.construct<TwoByteBuffer>();

        if (!ToUpperCase(cx, newChars.ref<TwoByteBuffer>(), chars, i, length,
                         &resultLength)) {
          return nullptr;
        }
      }
    } else {
      newChars.construct<TwoByteBuffer>();

      if (!ToUpperCase(cx, newChars.ref<TwoByteBuffer>(), chars, i, length,
                       &resultLength)) {
        return nullptr;
      }
    }
  }

  auto toString = [&](auto& chars) {
    return chars.toStringDontDeflate(cx, resultLength);
  };

  return newChars.mapNonEmpty(toString);
}

JSString* js::StringToUpperCase(JSContext* cx, HandleString string) {
  JSLinearString* linear = string->ensureLinear(cx);
  if (!linear) {
    return nullptr;
  }

  if (linear->hasLatin1Chars()) {
    return ToUpperCase<Latin1Char>(cx, linear);
  }
  return ToUpperCase<char16_t>(cx, linear);
}

static bool str_toUpperCase(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "toUpperCase");
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedString str(cx,
                   ToStringForStringFunction(cx, "toUpperCase", args.thisv()));
  if (!str) {
    return false;
  }

  JSString* result = StringToUpperCase(cx, str);
  if (!result) {
    return false;
  }

  args.rval().setString(result);
  return true;
}

#if JS_HAS_INTL_API
// String.prototype.toLocaleUpperCase is self-hosted when Intl is exposed,
// with core functionality performed by the intrinsic below.

bool js::intl_toLocaleUpperCase(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(args[0].isString());
  MOZ_ASSERT(args[1].isString());

  RootedString string(cx, args[0].toString());

  const char* locale = CaseMappingLocale(cx, args[1].toString());
  if (!locale) {
    return false;
  }

  // Call String.prototype.toUpperCase() for language independent casing.
  if (HasDefaultCasing(locale)) {
    JSString* str = js::StringToUpperCase(cx, string);
    if (!str) {
      return false;
    }

    args.rval().setString(str);
    return true;
  }

  AutoStableStringChars inputChars(cx);
  if (!inputChars.initTwoByte(cx, string)) {
    return false;
  }
  mozilla::Range<const char16_t> input = inputChars.twoByteRange();

  // Note: maximum case mapping length is three characters, so the result
  // length might be > INT32_MAX. ICU will fail in this case.
  static_assert(JSString::MAX_LENGTH <= INT32_MAX,
                "String length must fit in int32_t for ICU");

  static const size_t INLINE_CAPACITY = js::intl::INITIAL_CHAR_BUFFER_SIZE;

  intl::FormatBuffer<char16_t, INLINE_CAPACITY> buffer(cx);

  auto ok = mozilla::intl::String::ToLocaleUpperCase(locale, input, buffer);
  if (ok.isErr()) {
    intl::ReportInternalError(cx, ok.unwrapErr());
    return false;
  }

  JSString* result = buffer.toString(cx);
  if (!result) {
    return false;
  }

  args.rval().setString(result);
  return true;
}

#else

// When the Intl API is not exposed, String.prototype.toUpperCase is implemented
// in C++.
static bool str_toLocaleUpperCase(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype",
                                        "toLocaleUpperCase");
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedString str(
      cx, ToStringForStringFunction(cx, "toLocaleUpperCase", args.thisv()));
  if (!str) {
    return false;
  }

  /*
   * Forcefully ignore the first (or any) argument and return toUpperCase(),
   * ECMA has reserved that argument, presumably for defining the locale.
   */
  if (cx->runtime()->localeCallbacks &&
      cx->runtime()->localeCallbacks->localeToUpperCase) {
    RootedValue result(cx);
    if (!cx->runtime()->localeCallbacks->localeToUpperCase(cx, str, &result)) {
      return false;
    }

    args.rval().set(result);
    return true;
  }

  Rooted<JSLinearString*> linear(cx, str->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  JSString* result = StringToUpperCase(cx, linear);
  if (!result) {
    return false;
  }

  args.rval().setString(result);
  return true;
}

#endif  // JS_HAS_INTL_API

#if JS_HAS_INTL_API

// String.prototype.localeCompare is self-hosted when Intl functionality is
// exposed, and the only intrinsics it requires are provided in the
// implementation of Intl.Collator.

#else

// String.prototype.localeCompare is implemented in C++ (delegating to
// JSLocaleCallbacks) when Intl functionality is not exposed.
static bool str_localeCompare(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype",
                                        "localeCompare");
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedString str(
      cx, ToStringForStringFunction(cx, "localeCompare", args.thisv()));
  if (!str) {
    return false;
  }

  RootedString thatStr(cx, ToString<CanGC>(cx, args.get(0)));
  if (!thatStr) {
    return false;
  }

  if (cx->runtime()->localeCallbacks &&
      cx->runtime()->localeCallbacks->localeCompare) {
    RootedValue result(cx);
    if (!cx->runtime()->localeCallbacks->localeCompare(cx, str, thatStr,
                                                       &result)) {
      return false;
    }

    args.rval().set(result);
    return true;
  }

  int32_t result;
  if (!CompareStrings(cx, str, thatStr, &result)) {
    return false;
  }

  args.rval().setInt32(result);
  return true;
}

#endif  // JS_HAS_INTL_API

#if JS_HAS_INTL_API

// ES2017 draft rev 45e890512fd77add72cc0ee742785f9f6f6482de
// 21.1.3.12 String.prototype.normalize ( [ form ] )
//
// String.prototype.normalize is only implementable if ICU's normalization
// functionality is available.
static bool str_normalize(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "normalize");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  RootedString str(cx,
                   ToStringForStringFunction(cx, "normalize", args.thisv()));
  if (!str) {
    return false;
  }

  using NormalizationForm = mozilla::intl::String::NormalizationForm;

  NormalizationForm form;
  if (!args.hasDefined(0)) {
    // Step 3.
    form = NormalizationForm::NFC;
  } else {
    // Step 4.
    JSLinearString* formStr = ArgToLinearString(cx, args, 0);
    if (!formStr) {
      return false;
    }

    // Step 5.
    if (EqualStrings(formStr, cx->names().NFC)) {
      form = NormalizationForm::NFC;
    } else if (EqualStrings(formStr, cx->names().NFD)) {
      form = NormalizationForm::NFD;
    } else if (EqualStrings(formStr, cx->names().NFKC)) {
      form = NormalizationForm::NFKC;
    } else if (EqualStrings(formStr, cx->names().NFKD)) {
      form = NormalizationForm::NFKD;
    } else {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_INVALID_NORMALIZE_FORM);
      return false;
    }
  }

  // Latin-1 strings are already in Normalization Form C.
  if (form == NormalizationForm::NFC && str->hasLatin1Chars()) {
    // Step 7.
    args.rval().setString(str);
    return true;
  }

  // Step 6.
  AutoStableStringChars stableChars(cx);
  if (!stableChars.initTwoByte(cx, str)) {
    return false;
  }

  mozilla::Range<const char16_t> srcChars = stableChars.twoByteRange();

  static const size_t INLINE_CAPACITY = js::intl::INITIAL_CHAR_BUFFER_SIZE;

  intl::FormatBuffer<char16_t, INLINE_CAPACITY> buffer(cx);

  auto alreadyNormalized =
      mozilla::intl::String::Normalize(form, srcChars, buffer);
  if (alreadyNormalized.isErr()) {
    intl::ReportInternalError(cx, alreadyNormalized.unwrapErr());
    return false;
  }

  using AlreadyNormalized = mozilla::intl::String::AlreadyNormalized;

  // Return if the input string is already normalized.
  if (alreadyNormalized.unwrap() == AlreadyNormalized::Yes) {
    // Step 7.
    args.rval().setString(str);
    return true;
  }

  JSString* ns = buffer.toString(cx);
  if (!ns) {
    return false;
  }

  // Step 7.
  args.rval().setString(ns);
  return true;
}

#endif  // JS_HAS_INTL_API

#ifdef NIGHTLY_BUILD
/**
 * IsStringWellFormedUnicode ( string )
 * https://tc39.es/ecma262/#sec-isstringwellformedunicode
 */
static bool IsStringWellFormedUnicode(JSContext* cx, HandleString str,
                                      bool* isWellFormedOut) {
  MOZ_ASSERT(isWellFormedOut);
  *isWellFormedOut = false;

  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  // Latin1 chars are well-formed.
  if (linear->hasLatin1Chars()) {
    *isWellFormedOut = true;
    return true;
  }

  {
    AutoCheckCannotGC nogc;
    size_t len = linear->length();
    *isWellFormedOut =
        Utf16ValidUpTo(Span{linear->twoByteChars(nogc), len}) == len;
  }
  return true;
}

/**
 * Well-Formed Unicode Strings (Stage 3 proposal)
 *
 * String.prototype.isWellFormed
 * https://tc39.es/proposal-is-usv-string/#sec-string.prototype.iswellformed
 */
static bool str_isWellFormed(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "isWellFormed");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. Let O be ? RequireObjectCoercible(this value).
  // Step 2. Let S be ? ToString(O).
  RootedString str(cx,
                   ToStringForStringFunction(cx, "isWellFormed", args.thisv()));
  if (!str) {
    return false;
  }

  // Step 3. Return IsStringWellFormedUnicode(S).
  bool isWellFormed;
  if (!IsStringWellFormedUnicode(cx, str, &isWellFormed)) {
    return false;
  }

  args.rval().setBoolean(isWellFormed);
  return true;
}

/**
 * Well-Formed Unicode Strings (Stage 3 proposal)
 *
 * String.prototype.toWellFormed
 * https://tc39.es/proposal-is-usv-string/#sec-string.prototype.towellformed
 */
static bool str_toWellFormed(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "toWellFormed");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1. Let O be ? RequireObjectCoercible(this value).
  // Step 2. Let S be ? ToString(O).
  RootedString str(cx,
                   ToStringForStringFunction(cx, "toWellFormed", args.thisv()));
  if (!str) {
    return false;
  }

  // If the string itself is well-formed, return it.
  bool isWellFormed;
  if (!IsStringWellFormedUnicode(cx, str, &isWellFormed)) {
    return false;
  }
  if (isWellFormed) {
    args.rval().setString(str);
    return true;
  }

  // Step 3. Let strLen be the length of S.
  size_t len = str->length();

  // Step 4-6
  auto buffer = cx->make_pod_arena_array<char16_t>(js::StringBufferArena, len);
  if (!buffer) {
    return false;
  }

  {
    AutoCheckCannotGC nogc;
    JSLinearString* linear = str->ensureLinear(cx);
    PodCopy(buffer.get(), linear->twoByteChars(nogc), len);
    EnsureUtf16ValiditySpan(Span{buffer.get(), len});
  }

  JSString* result = NewString<CanGC>(cx, std::move(buffer), len);
  if (!result) {
    return false;
  }

  // Step 7. Return result.
  args.rval().setString(result);
  return true;
}
#endif  // NIGHTLY_BUILD

static bool str_charAt(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "charAt");
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedString str(cx);
  size_t i;
  if (args.thisv().isString() && args.length() != 0 && args[0].isInt32()) {
    str = args.thisv().toString();
    i = size_t(args[0].toInt32());
    if (i >= str->length()) {
      goto out_of_range;
    }
  } else {
    str = ToStringForStringFunction(cx, "charAt", args.thisv());
    if (!str) {
      return false;
    }

    double d = 0.0;
    if (args.length() > 0 && !ToInteger(cx, args[0], &d)) {
      return false;
    }

    if (d < 0 || str->length() <= d) {
      goto out_of_range;
    }
    i = size_t(d);
  }

  str = cx->staticStrings().getUnitStringForElement(cx, str, i);
  if (!str) {
    return false;
  }
  args.rval().setString(str);
  return true;

out_of_range:
  args.rval().setString(cx->runtime()->emptyString);
  return true;
}

bool js::str_charCodeAt_impl(JSContext* cx, HandleString string,
                             HandleValue index, MutableHandleValue res) {
  size_t i;
  if (index.isInt32()) {
    i = index.toInt32();
    if (i >= string->length()) {
      goto out_of_range;
    }
  } else {
    double d = 0.0;
    if (!ToInteger(cx, index, &d)) {
      return false;
    }
    // check whether d is negative as size_t is unsigned
    if (d < 0 || string->length() <= d) {
      goto out_of_range;
    }
    i = size_t(d);
  }
  char16_t c;
  if (!string->getChar(cx, i, &c)) {
    return false;
  }
  res.setInt32(c);
  return true;

out_of_range:
  res.setNaN();
  return true;
}

bool js::str_charCodeAt(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "charCodeAt");
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedString str(cx);
  RootedValue index(cx);
  if (args.thisv().isString()) {
    str = args.thisv().toString();
  } else {
    str = ToStringForStringFunction(cx, "charCodeAt", args.thisv());
    if (!str) {
      return false;
    }
  }
  if (args.length() != 0) {
    index = args[0];
  } else {
    index.setInt32(0);
  }

  return js::str_charCodeAt_impl(cx, str, index, args.rval());
}

/*
 * Boyer-Moore-Horspool superlinear search for pat:patlen in text:textlen.
 * The patlen argument must be positive and no greater than sBMHPatLenMax.
 *
 * Return the index of pat in text, or -1 if not found.
 */
static const uint32_t sBMHCharSetSize = 256; /* ISO-Latin-1 */
static const uint32_t sBMHPatLenMax = 255;   /* skip table element is uint8_t */
static const int sBMHBadPattern =
    -2; /* return value if pat is not ISO-Latin-1 */

template <typename TextChar, typename PatChar>
static int BoyerMooreHorspool(const TextChar* text, uint32_t textLen,
                              const PatChar* pat, uint32_t patLen) {
  MOZ_ASSERT(0 < patLen && patLen <= sBMHPatLenMax);

  uint8_t skip[sBMHCharSetSize];
  for (uint32_t i = 0; i < sBMHCharSetSize; i++) {
    skip[i] = uint8_t(patLen);
  }

  uint32_t patLast = patLen - 1;
  for (uint32_t i = 0; i < patLast; i++) {
    char16_t c = pat[i];
    if (c >= sBMHCharSetSize) {
      return sBMHBadPattern;
    }
    skip[c] = uint8_t(patLast - i);
  }

  for (uint32_t k = patLast; k < textLen;) {
    for (uint32_t i = k, j = patLast;; i--, j--) {
      if (text[i] != pat[j]) {
        break;
      }
      if (j == 0) {
        return static_cast<int>(i); /* safe: max string size */
      }
    }

    char16_t c = text[k];
    k += (c >= sBMHCharSetSize) ? patLen : skip[c];
  }
  return -1;
}

template <typename TextChar, typename PatChar>
struct MemCmp {
  using Extent = uint32_t;
  static MOZ_ALWAYS_INLINE Extent computeExtent(const PatChar*,
                                                uint32_t patLen) {
    return (patLen - 2) * sizeof(PatChar);
  }
  static MOZ_ALWAYS_INLINE bool match(const PatChar* p, const TextChar* t,
                                      Extent extent) {
    MOZ_ASSERT(sizeof(TextChar) == sizeof(PatChar));
    return memcmp(p, t, extent) == 0;
  }
};

template <typename TextChar, typename PatChar>
struct ManualCmp {
  using Extent = const PatChar*;
  static MOZ_ALWAYS_INLINE Extent computeExtent(const PatChar* pat,
                                                uint32_t patLen) {
    return pat + patLen;
  }
  static MOZ_ALWAYS_INLINE bool match(const PatChar* p, const TextChar* t,
                                      Extent extent) {
    for (; p != extent; ++p, ++t) {
      if (*p != *t) {
        return false;
      }
    }
    return true;
  }
};

template <class InnerMatch, typename TextChar, typename PatChar>
static int Matcher(const TextChar* text, uint32_t textlen, const PatChar* pat,
                   uint32_t patlen) {
  MOZ_ASSERT(patlen > 1);

  const typename InnerMatch::Extent extent =
      InnerMatch::computeExtent(pat, patlen);

  uint32_t i = 0;
  uint32_t n = textlen - patlen + 1;

  while (i < n) {
    const TextChar* pos;

    // This is a bit awkward. Consider the case where we're searching "abcdef"
    // for "def". n will be 4, because we know in advance that the last place we
    // can *start* a successful search will be at 'd'. However, if we just use n
    // - i, then our first search will be looking through "abcd" for "de",
    // because our memchr2xN functions search for two characters at a time. So
    // we just have to compensate by adding 1. This will never exceed textlen
    // because we know patlen is at least two.
    size_t searchLen = n - i + 1;
    if (sizeof(TextChar) == 1) {
      MOZ_ASSERT(pat[0] <= 0xff);
      pos = (TextChar*)SIMD::memchr2x8((char*)text + i, pat[0], pat[1],
                                       searchLen);
    } else {
      pos = (TextChar*)SIMD::memchr2x16((char16_t*)(text + i), char16_t(pat[0]),
                                        char16_t(pat[1]), searchLen);
    }

    if (pos == nullptr) {
      return -1;
    }

    i = static_cast<uint32_t>(pos - text);
    const uint32_t inlineLookaheadChars = 2;
    if (InnerMatch::match(pat + inlineLookaheadChars,
                          text + i + inlineLookaheadChars, extent)) {
      return i;
    }

    i += 1;
  }
  return -1;
}

template <typename TextChar, typename PatChar>
static MOZ_ALWAYS_INLINE int StringMatch(const TextChar* text, uint32_t textLen,
                                         const PatChar* pat, uint32_t patLen) {
  if (patLen == 0) {
    return 0;
  }
  if (textLen < patLen) {
    return -1;
  }

  if (sizeof(TextChar) == 1 && sizeof(PatChar) > 1 && pat[0] > 0xff) {
    return -1;
  }

  if (patLen == 1) {
    const TextChar* pos;
    if (sizeof(TextChar) == 1) {
      MOZ_ASSERT(pat[0] <= 0xff);
      pos = (TextChar*)SIMD::memchr8((char*)text, pat[0], textLen);
    } else {
      pos =
          (TextChar*)SIMD::memchr16((char16_t*)text, char16_t(pat[0]), textLen);
    }

    if (pos == nullptr) {
      return -1;
    }

    return pos - text;
  }

  // We use a fast two-character-wide search in Matcher below, so we need to
  // validate that pat[1] isn't outside the latin1 range up front if the
  // sizes are different.
  if (sizeof(TextChar) == 1 && sizeof(PatChar) > 1 && pat[1] > 0xff) {
    return -1;
  }

  /*
   * If the text or pattern string is short, BMH will be more expensive than
   * the basic linear scan due to initialization cost and a more complex loop
   * body. While the correct threshold is input-dependent, we can make a few
   * conservative observations:
   *  - When |textLen| is "big enough", the initialization time will be
   *    proportionally small, so the worst-case slowdown is minimized.
   *  - When |patLen| is "too small", even the best case for BMH will be
   *    slower than a simple scan for large |textLen| due to the more complex
   *    loop body of BMH.
   * From this, the values for "big enough" and "too small" are determined
   * empirically. See bug 526348.
   */
  if (textLen >= 512 && patLen >= 11 && patLen <= sBMHPatLenMax) {
    int index = BoyerMooreHorspool(text, textLen, pat, patLen);
    if (index != sBMHBadPattern) {
      return index;
    }
  }

  /*
   * For big patterns with large potential overlap we want the SIMD-optimized
   * speed of memcmp. For small patterns, a simple loop is faster. We also can't
   * use memcmp if one of the strings is TwoByte and the other is Latin-1.
   */
  return (patLen > 128 && std::is_same_v<TextChar, PatChar>)
             ? Matcher<MemCmp<TextChar, PatChar>, TextChar, PatChar>(
                   text, textLen, pat, patLen)
             : Matcher<ManualCmp<TextChar, PatChar>, TextChar, PatChar>(
                   text, textLen, pat, patLen);
}

static int32_t StringMatch(JSLinearString* text, JSLinearString* pat,
                           uint32_t start = 0) {
  MOZ_ASSERT(start <= text->length());
  uint32_t textLen = text->length() - start;
  uint32_t patLen = pat->length();

  int match;
  AutoCheckCannotGC nogc;
  if (text->hasLatin1Chars()) {
    const Latin1Char* textChars = text->latin1Chars(nogc) + start;
    if (pat->hasLatin1Chars()) {
      match = StringMatch(textChars, textLen, pat->latin1Chars(nogc), patLen);
    } else {
      match = StringMatch(textChars, textLen, pat->twoByteChars(nogc), patLen);
    }
  } else {
    const char16_t* textChars = text->twoByteChars(nogc) + start;
    if (pat->hasLatin1Chars()) {
      match = StringMatch(textChars, textLen, pat->latin1Chars(nogc), patLen);
    } else {
      match = StringMatch(textChars, textLen, pat->twoByteChars(nogc), patLen);
    }
  }

  return (match == -1) ? -1 : start + match;
}

static const size_t sRopeMatchThresholdRatioLog2 = 4;

int js::StringFindPattern(JSLinearString* text, JSLinearString* pat,
                          size_t start) {
  return StringMatch(text, pat, start);
}

typedef Vector<JSLinearString*, 16, SystemAllocPolicy> LinearStringVector;

template <typename TextChar, typename PatChar>
static int RopeMatchImpl(const AutoCheckCannotGC& nogc,
                         LinearStringVector& strings, const PatChar* pat,
                         size_t patLen) {
  /* Absolute offset from the beginning of the logical text string. */
  int pos = 0;

  for (JSLinearString** outerp = strings.begin(); outerp != strings.end();
       ++outerp) {
    /* Try to find a match within 'outer'. */
    JSLinearString* outer = *outerp;
    const TextChar* chars = outer->chars<TextChar>(nogc);
    size_t len = outer->length();
    int matchResult = StringMatch(chars, len, pat, patLen);
    if (matchResult != -1) {
      /* Matched! */
      return pos + matchResult;
    }

    /* Try to find a match starting in 'outer' and running into other nodes. */
    const TextChar* const text = chars + (patLen > len ? 0 : len - patLen + 1);
    const TextChar* const textend = chars + len;
    const PatChar p0 = *pat;
    const PatChar* const p1 = pat + 1;
    const PatChar* const patend = pat + patLen;
    for (const TextChar* t = text; t != textend;) {
      if (*t++ != p0) {
        continue;
      }

      JSLinearString** innerp = outerp;
      const TextChar* ttend = textend;
      const TextChar* tt = t;
      for (const PatChar* pp = p1; pp != patend; ++pp, ++tt) {
        while (tt == ttend) {
          if (++innerp == strings.end()) {
            return -1;
          }

          JSLinearString* inner = *innerp;
          tt = inner->chars<TextChar>(nogc);
          ttend = tt + inner->length();
        }
        if (*pp != *tt) {
          goto break_continue;
        }
      }

      /* Matched! */
      return pos + (t - chars) - 1; /* -1 because of *t++ above */

    break_continue:;
    }

    pos += len;
  }

  return -1;
}

/*
 * RopeMatch takes the text to search and the pattern to search for in the text.
 * RopeMatch returns false on OOM and otherwise returns the match index through
 * the 'match' outparam (-1 for not found).
 */
static bool RopeMatch(JSContext* cx, JSRope* text, JSLinearString* pat,
                      int* match) {
  uint32_t patLen = pat->length();
  if (patLen == 0) {
    *match = 0;
    return true;
  }
  if (text->length() < patLen) {
    *match = -1;
    return true;
  }

  /*
   * List of leaf nodes in the rope. If we run out of memory when trying to
   * append to this list, we can still fall back to StringMatch, so use the
   * system allocator so we don't report OOM in that case.
   */
  LinearStringVector strings;

  /*
   * We don't want to do rope matching if there is a poor node-to-char ratio,
   * since this means spending a lot of time in the match loop below. We also
   * need to build the list of leaf nodes. Do both here: iterate over the
   * nodes so long as there are not too many.
   *
   * We also don't use rope matching if the rope contains both Latin-1 and
   * TwoByte nodes, to simplify the match algorithm.
   */
  {
    size_t threshold = text->length() >> sRopeMatchThresholdRatioLog2;
    StringSegmentRange r(cx);
    if (!r.init(text)) {
      return false;
    }

    bool textIsLatin1 = text->hasLatin1Chars();
    while (!r.empty()) {
      if (threshold-- == 0 || r.front()->hasLatin1Chars() != textIsLatin1 ||
          !strings.append(r.front())) {
        JSLinearString* linear = text->ensureLinear(cx);
        if (!linear) {
          return false;
        }

        *match = StringMatch(linear, pat);
        return true;
      }
      if (!r.popFront()) {
        return false;
      }
    }
  }

  AutoCheckCannotGC nogc;
  if (text->hasLatin1Chars()) {
    if (pat->hasLatin1Chars()) {
      *match = RopeMatchImpl<Latin1Char>(nogc, strings, pat->latin1Chars(nogc),
                                         patLen);
    } else {
      *match = RopeMatchImpl<Latin1Char>(nogc, strings, pat->twoByteChars(nogc),
                                         patLen);
    }
  } else {
    if (pat->hasLatin1Chars()) {
      *match = RopeMatchImpl<char16_t>(nogc, strings, pat->latin1Chars(nogc),
                                       patLen);
    } else {
      *match = RopeMatchImpl<char16_t>(nogc, strings, pat->twoByteChars(nogc),
                                       patLen);
    }
  }

  return true;
}

static MOZ_ALWAYS_INLINE bool ReportErrorIfFirstArgIsRegExp(
    JSContext* cx, const CallArgs& args) {
  // Only call IsRegExp if the first argument is definitely an object, so we
  // don't pay the cost of an additional function call in the common case.
  if (args.length() == 0 || !args[0].isObject()) {
    return true;
  }

  bool isRegExp;
  if (!IsRegExp(cx, args[0], &isRegExp)) {
    return false;
  }

  if (isRegExp) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INVALID_ARG_TYPE, "first", "",
                              "Regular Expression");
    return false;
  }
  return true;
}

// ES2018 draft rev de77aaeffce115deaf948ed30c7dbe4c60983c0c
// 21.1.3.7 String.prototype.includes ( searchString [ , position ] )
bool js::str_includes(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "includes");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  RootedString str(cx, ToStringForStringFunction(cx, "includes", args.thisv()));
  if (!str) {
    return false;
  }

  // Steps 3-4.
  if (!ReportErrorIfFirstArgIsRegExp(cx, args)) {
    return false;
  }

  // Step 5.
  Rooted<JSLinearString*> searchStr(cx, ArgToLinearString(cx, args, 0));
  if (!searchStr) {
    return false;
  }

  // Step 6.
  uint32_t pos = 0;
  if (args.hasDefined(1)) {
    if (args[1].isInt32()) {
      int i = args[1].toInt32();
      pos = (i < 0) ? 0U : uint32_t(i);
    } else {
      double d;
      if (!ToInteger(cx, args[1], &d)) {
        return false;
      }
      pos = uint32_t(std::min(std::max(d, 0.0), double(UINT32_MAX)));
    }
  }

  // Step 7.
  uint32_t textLen = str->length();

  // Step 8.
  uint32_t start = std::min(pos, textLen);

  // Steps 9-10.
  JSLinearString* text = str->ensureLinear(cx);
  if (!text) {
    return false;
  }

  args.rval().setBoolean(StringMatch(text, searchStr, start) != -1);
  return true;
}

/* ES6 20120927 draft 15.5.4.7. */
bool js::str_indexOf(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "indexOf");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1, 2, and 3
  RootedString str(cx, ToStringForStringFunction(cx, "indexOf", args.thisv()));
  if (!str) {
    return false;
  }

  // Steps 4 and 5
  Rooted<JSLinearString*> searchStr(cx, ArgToLinearString(cx, args, 0));
  if (!searchStr) {
    return false;
  }

  // Steps 6 and 7
  uint32_t pos = 0;
  if (args.hasDefined(1)) {
    if (args[1].isInt32()) {
      int i = args[1].toInt32();
      pos = (i < 0) ? 0U : uint32_t(i);
    } else {
      double d;
      if (!ToInteger(cx, args[1], &d)) {
        return false;
      }
      pos = uint32_t(std::min(std::max(d, 0.0), double(UINT32_MAX)));
    }
  }

  // Step 8
  uint32_t textLen = str->length();

  // Step 9
  uint32_t start = std::min(pos, textLen);

  if (str == searchStr) {
    // AngularJS often invokes "false".indexOf("false"). This check should
    // be cheap enough to not hurt anything else.
    args.rval().setInt32(start == 0 ? 0 : -1);
    return true;
  }

  // Steps 10 and 11
  JSLinearString* text = str->ensureLinear(cx);
  if (!text) {
    return false;
  }

  args.rval().setInt32(StringMatch(text, searchStr, start));
  return true;
}

bool js::StringIndexOf(JSContext* cx, HandleString string,
                       HandleString searchString, int32_t* result) {
  if (string == searchString) {
    *result = 0;
    return true;
  }

  JSLinearString* text = string->ensureLinear(cx);
  if (!text) {
    return false;
  }

  JSLinearString* searchStr = searchString->ensureLinear(cx);
  if (!searchStr) {
    return false;
  }

  *result = StringMatch(text, searchStr, 0);
  return true;
}

template <typename TextChar, typename PatChar>
static int32_t LastIndexOfImpl(const TextChar* text, size_t textLen,
                               const PatChar* pat, size_t patLen,
                               size_t start) {
  MOZ_ASSERT(patLen > 0);
  MOZ_ASSERT(patLen <= textLen);
  MOZ_ASSERT(start <= textLen - patLen);

  const PatChar p0 = *pat;
  const PatChar* patNext = pat + 1;
  const PatChar* patEnd = pat + patLen;

  for (const TextChar* t = text + start; t >= text; --t) {
    if (*t == p0) {
      const TextChar* t1 = t + 1;
      for (const PatChar* p1 = patNext; p1 < patEnd; ++p1, ++t1) {
        if (*t1 != *p1) {
          goto break_continue;
        }
      }

      return static_cast<int32_t>(t - text);
    }
  break_continue:;
  }

  return -1;
}

// ES2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e
// 21.1.3.9 String.prototype.lastIndexOf ( searchString [ , position ] )
static bool str_lastIndexOf(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "lastIndexOf");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  RootedString str(cx,
                   ToStringForStringFunction(cx, "lastIndexOf", args.thisv()));
  if (!str) {
    return false;
  }

  // Step 3.
  Rooted<JSLinearString*> searchStr(cx, ArgToLinearString(cx, args, 0));
  if (!searchStr) {
    return false;
  }

  // Step 6.
  size_t len = str->length();

  // Step 8.
  size_t searchLen = searchStr->length();

  // Steps 4-5, 7.
  int start = len - searchLen;  // Start searching here
  if (args.hasDefined(1)) {
    if (args[1].isInt32()) {
      int i = args[1].toInt32();
      if (i <= 0) {
        start = 0;
      } else if (i < start) {
        start = i;
      }
    } else {
      double d;
      if (!ToNumber(cx, args[1], &d)) {
        return false;
      }
      if (!std::isnan(d)) {
        d = JS::ToInteger(d);
        if (d <= 0) {
          start = 0;
        } else if (d < start) {
          start = int(d);
        }
      }
    }
  }

  if (str == searchStr) {
    args.rval().setInt32(0);
    return true;
  }

  if (searchLen > len) {
    args.rval().setInt32(-1);
    return true;
  }

  if (searchLen == 0) {
    args.rval().setInt32(start);
    return true;
  }
  MOZ_ASSERT(0 <= start && size_t(start) < len);

  JSLinearString* text = str->ensureLinear(cx);
  if (!text) {
    return false;
  }

  // Step 9.
  int32_t res;
  AutoCheckCannotGC nogc;
  if (text->hasLatin1Chars()) {
    const Latin1Char* textChars = text->latin1Chars(nogc);
    if (searchStr->hasLatin1Chars()) {
      res = LastIndexOfImpl(textChars, len, searchStr->latin1Chars(nogc),
                            searchLen, start);
    } else {
      res = LastIndexOfImpl(textChars, len, searchStr->twoByteChars(nogc),
                            searchLen, start);
    }
  } else {
    const char16_t* textChars = text->twoByteChars(nogc);
    if (searchStr->hasLatin1Chars()) {
      res = LastIndexOfImpl(textChars, len, searchStr->latin1Chars(nogc),
                            searchLen, start);
    } else {
      res = LastIndexOfImpl(textChars, len, searchStr->twoByteChars(nogc),
                            searchLen, start);
    }
  }

  args.rval().setInt32(res);
  return true;
}

// ES2018 draft rev de77aaeffce115deaf948ed30c7dbe4c60983c0c
// 21.1.3.20 String.prototype.startsWith ( searchString [ , position ] )
bool js::str_startsWith(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "startsWith");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  RootedString str(cx,
                   ToStringForStringFunction(cx, "startsWith", args.thisv()));
  if (!str) {
    return false;
  }

  // Steps 3-4.
  if (!ReportErrorIfFirstArgIsRegExp(cx, args)) {
    return false;
  }

  // Step 5.
  Rooted<JSLinearString*> searchStr(cx, ArgToLinearString(cx, args, 0));
  if (!searchStr) {
    return false;
  }

  // Step 6.
  uint32_t pos = 0;
  if (args.hasDefined(1)) {
    if (args[1].isInt32()) {
      int i = args[1].toInt32();
      pos = (i < 0) ? 0U : uint32_t(i);
    } else {
      double d;
      if (!ToInteger(cx, args[1], &d)) {
        return false;
      }
      pos = uint32_t(std::min(std::max(d, 0.0), double(UINT32_MAX)));
    }
  }

  // Step 7.
  uint32_t textLen = str->length();

  // Step 8.
  uint32_t start = std::min(pos, textLen);

  // Step 9.
  uint32_t searchLen = searchStr->length();

  // Step 10.
  if (searchLen + start < searchLen || searchLen + start > textLen) {
    args.rval().setBoolean(false);
    return true;
  }

  // Steps 11-12.
  JSLinearString* text = str->ensureLinear(cx);
  if (!text) {
    return false;
  }

  args.rval().setBoolean(HasSubstringAt(text, searchStr, start));
  return true;
}

bool js::StringStartsWith(JSContext* cx, HandleString string,
                          HandleString searchString, bool* result) {
  if (searchString->length() > string->length()) {
    *result = false;
    return true;
  }

  JSLinearString* str = string->ensureLinear(cx);
  if (!str) {
    return false;
  }

  JSLinearString* searchStr = searchString->ensureLinear(cx);
  if (!searchStr) {
    return false;
  }

  *result = HasSubstringAt(str, searchStr, 0);
  return true;
}

// ES2018 draft rev de77aaeffce115deaf948ed30c7dbe4c60983c0c
// 21.1.3.6 String.prototype.endsWith ( searchString [ , endPosition ] )
bool js::str_endsWith(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "endsWith");
  CallArgs args = CallArgsFromVp(argc, vp);

  // Steps 1-2.
  RootedString str(cx, ToStringForStringFunction(cx, "endsWith", args.thisv()));
  if (!str) {
    return false;
  }

  // Steps 3-4.
  if (!ReportErrorIfFirstArgIsRegExp(cx, args)) {
    return false;
  }

  // Step 5.
  Rooted<JSLinearString*> searchStr(cx, ArgToLinearString(cx, args, 0));
  if (!searchStr) {
    return false;
  }

  // Step 6.
  uint32_t textLen = str->length();

  // Step 7.
  uint32_t pos = textLen;
  if (args.hasDefined(1)) {
    if (args[1].isInt32()) {
      int i = args[1].toInt32();
      pos = (i < 0) ? 0U : uint32_t(i);
    } else {
      double d;
      if (!ToInteger(cx, args[1], &d)) {
        return false;
      }
      pos = uint32_t(std::min(std::max(d, 0.0), double(UINT32_MAX)));
    }
  }

  // Step 8.
  uint32_t end = std::min(pos, textLen);

  // Step 9.
  uint32_t searchLen = searchStr->length();

  // Step 11 (reordered).
  if (searchLen > end) {
    args.rval().setBoolean(false);
    return true;
  }

  // Step 10.
  uint32_t start = end - searchLen;

  // Steps 12-13.
  JSLinearString* text = str->ensureLinear(cx);
  if (!text) {
    return false;
  }

  args.rval().setBoolean(HasSubstringAt(text, searchStr, start));
  return true;
}

bool js::StringEndsWith(JSContext* cx, HandleString string,
                        HandleString searchString, bool* result) {
  if (searchString->length() > string->length()) {
    *result = false;
    return true;
  }

  JSLinearString* str = string->ensureLinear(cx);
  if (!str) {
    return false;
  }

  JSLinearString* searchStr = searchString->ensureLinear(cx);
  if (!searchStr) {
    return false;
  }

  uint32_t start = str->length() - searchStr->length();

  *result = HasSubstringAt(str, searchStr, start);
  return true;
}

template <typename CharT>
static void TrimString(const CharT* chars, bool trimStart, bool trimEnd,
                       size_t length, size_t* pBegin, size_t* pEnd) {
  size_t begin = 0, end = length;

  if (trimStart) {
    while (begin < length && unicode::IsSpace(chars[begin])) {
      ++begin;
    }
  }

  if (trimEnd) {
    while (end > begin && unicode::IsSpace(chars[end - 1])) {
      --end;
    }
  }

  *pBegin = begin;
  *pEnd = end;
}

static bool TrimString(JSContext* cx, const CallArgs& args, const char* funName,
                       bool trimStart, bool trimEnd) {
  JSString* str = ToStringForStringFunction(cx, funName, args.thisv());
  if (!str) {
    return false;
  }

  JSLinearString* linear = str->ensureLinear(cx);
  if (!linear) {
    return false;
  }

  size_t length = linear->length();
  size_t begin, end;
  if (linear->hasLatin1Chars()) {
    AutoCheckCannotGC nogc;
    TrimString(linear->latin1Chars(nogc), trimStart, trimEnd, length, &begin,
               &end);
  } else {
    AutoCheckCannotGC nogc;
    TrimString(linear->twoByteChars(nogc), trimStart, trimEnd, length, &begin,
               &end);
  }

  JSLinearString* result = NewDependentString(cx, linear, begin, end - begin);
  if (!result) {
    return false;
  }

  args.rval().setString(result);
  return true;
}

static bool str_trim(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "trim");
  CallArgs args = CallArgsFromVp(argc, vp);
  return TrimString(cx, args, "trim", true, true);
}

static bool str_trimStart(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "trimStart");
  CallArgs args = CallArgsFromVp(argc, vp);
  return TrimString(cx, args, "trimStart", true, false);
}

static bool str_trimEnd(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "String.prototype", "trimEnd");
  CallArgs args = CallArgsFromVp(argc, vp);
  return TrimString(cx, args, "trimEnd", false, true);
}

// Utility for building a rope (lazy concatenation) of strings.
class RopeBuilder {
  JSContext* cx;
  RootedString res;

  RopeBuilder(const RopeBuilder& other) = delete;
  void operator=(const RopeBuilder& other) = delete;

 public:
  explicit RopeBuilder(JSContext* cx)
      : cx(cx), res(cx, cx->runtime()->emptyString) {}

  inline bool append(HandleString str) {
    res = ConcatStrings<CanGC>(cx, res, str);
    return !!res;
  }

  inline JSString* result() { return res; }
};

namespace {

template <typename CharT>
static uint32_t FindDollarIndex(const CharT* chars, size_t length) {
  if (const CharT* p = js_strchr_limit(chars, '$', chars + length)) {
    uint32_t dollarIndex = p - chars;
    MOZ_ASSERT(dollarIndex < length);
    return dollarIndex;
  }
  return UINT32_MAX;
}

} /* anonymous namespace */

/*
 * Constructs a result string that looks like:
 *
 *      newstring = string[:matchStart] + repstr + string[matchEnd:]
 */
static JSString* BuildFlatReplacement(JSContext* cx, HandleString textstr,
                                      Handle<JSLinearString*> repstr,
                                      size_t matchStart, size_t patternLength) {
  size_t matchEnd = matchStart + patternLength;

  RootedString resultStr(cx, NewDependentString(cx, textstr, 0, matchStart));
  if (!resultStr) {
    return nullptr;
  }

  resultStr = ConcatStrings<CanGC>(cx, resultStr, repstr);
  if (!resultStr) {
    return nullptr;
  }

  MOZ_ASSERT(textstr->length() >= matchEnd);
  RootedString rest(cx, NewDependentString(cx, textstr, matchEnd,
                                           textstr->length() - matchEnd));
  if (!rest) {
    return nullptr;
  }

  return ConcatStrings<CanGC>(cx, resultStr, rest);
}

static JSString* BuildFlatRopeReplacement(JSContext* cx, HandleString textstr,
                                          Handle<JSLinearString*> repstr,
                                          size_t match, size_t patternLength) {
  MOZ_ASSERT(textstr->isRope());

  size_t matchEnd = match + patternLength;

  /*
   * If we are replacing over a rope, avoid flattening it by iterating
   * through it, building a new rope.
   */
  StringSegmentRange r(cx);
  if (!r.init(textstr)) {
    return nullptr;
  }

  RopeBuilder builder(cx);

  /*
   * Special case when the pattern string is '', which matches to the
   * head of the string and doesn't overlap with any component of the rope.
   */
  if (patternLength == 0) {
    MOZ_ASSERT(match == 0);
    if (!builder.append(repstr)) {
      return nullptr;
    }
  }

  size_t pos = 0;
  while (!r.empty()) {
    RootedString str(cx, r.front());
    size_t len = str->length();
    size_t strEnd = pos + len;
    if (pos < matchEnd && strEnd > match) {
      /*
       * We need to special-case any part of the rope that overlaps
       * with the replacement string.
       */
      if (match >= pos) {
        /*
         * If this part of the rope overlaps with the left side of
         * the pattern, then it must be the only one to overlap with
         * the first character in the pattern, so we include the
         * replacement string here.
         */
        RootedString leftSide(cx, NewDependentString(cx, str, 0, match - pos));
        if (!leftSide || !builder.append(leftSide) || !builder.append(repstr)) {
          return nullptr;
        }
      }

      /*
       * If str runs off the end of the matched string, append the
       * last part of str.
       */
      if (strEnd > matchEnd) {
        RootedString rightSide(
            cx, NewDependentString(cx, str, matchEnd - pos, strEnd - matchEnd));
        if (!rightSide || !builder.append(rightSide)) {
          return nullptr;
        }
      }
    } else {
      if (!builder.append(str)) {
        return nullptr;
      }
    }
    pos += str->length();
    if (!r.popFront()) {
      return nullptr;
    }
  }

  return builder.result();
}

template <typename CharT>
static bool AppendDollarReplacement(StringBuffer& newReplaceChars,
                                    size_t firstDollarIndex, size_t matchStart,
                                    size_t matchLimit, JSLinearString* text,
                                    const CharT* repChars, size_t repLength) {
  MOZ_ASSERT(firstDollarIndex < repLength);
  MOZ_ASSERT(matchStart <= matchLimit);
  MOZ_ASSERT(matchLimit <= text->length());

  // Move the pre-dollar chunk in bulk.
  if (!newReplaceChars.append(repChars, firstDollarIndex)) {
    return false;
  }

  // Move the rest char-by-char, interpreting dollars as we encounter them.
  const CharT* repLimit = repChars + repLength;
  for (const CharT* it = repChars + firstDollarIndex; it < repLimit; ++it) {
    if (*it != '$' || it == repLimit - 1) {
      if (!newReplaceChars.append(*it)) {
        return false;
      }
      continue;
    }

    switch (*(it + 1)) {
      case '$':
        // Eat one of the dollars.
        if (!newReplaceChars.append(*it)) {
          return false;
        }
        break;
      case '&':
        if (!newReplaceChars.appendSubstring(text, matchStart,
                                             matchLimit - matchStart)) {
          return false;
        }
        break;
      case '`':
        if (!newReplaceChars.appendSubstring(text, 0, matchStart)) {
          return false;
        }
        break;
      case '\'':
        if (!newReplaceChars.appendSubstring(text, matchLimit,
                                             text->length() - matchLimit)) {
          return false;
        }
        break;
      default:
        // The dollar we saw was not special (no matter what its mother told
        // it).
        if (!newReplaceChars.append(*it)) {
          return false;
        }
        continue;
    }
    ++it;  // We always eat an extra char in the above switch.
  }

  return true;
}

/*
 * Perform a linear-scan dollar substitution on the replacement text.
 */
static JSLinearString* InterpretDollarReplacement(
    JSContext* cx, HandleString textstrArg, Handle<JSLinearString*> repstr,
    uint32_t firstDollarIndex, size_t matchStart, size_t patternLength) {
  Rooted<JSLinearString*> textstr(cx, textstrArg->ensureLinear(cx));
  if (!textstr) {
    return nullptr;
  }

  size_t matchLimit = matchStart + patternLength;

  /*
   * Most probably:
   *
   *      len(newstr) >= len(orig) - len(match) + len(replacement)
   *
   * Note that dollar vars _could_ make the resulting text smaller than this.
   */
  JSStringBuilder newReplaceChars(cx);
  if (repstr->hasTwoByteChars() && !newReplaceChars.ensureTwoByteChars()) {
    return nullptr;
  }

  if (!newReplaceChars.reserve(textstr->length() - patternLength +
                               repstr->length())) {
    return nullptr;
  }

  bool res;
  if (repstr->hasLatin1Chars()) {
    AutoCheckCannotGC nogc;
    res = AppendDollarReplacement(newReplaceChars, firstDollarIndex, matchStart,
                                  matchLimit, textstr,
                                  repstr->latin1Chars(nogc), repstr->length());
  } else {
    AutoCheckCannotGC nogc;
    res = AppendDollarReplacement(newReplaceChars, firstDollarIndex, matchStart,
                                  matchLimit, textstr,
                                  repstr->twoByteChars(nogc), repstr->length());
  }
  if (!res) {
    return nullptr;
  }

  return newReplaceChars.finishString();
}

template <typename StrChar, typename RepChar>
static bool StrFlatReplaceGlobal(JSContext* cx, JSLinearString* str,
                                 JSLinearString* pat, JSLinearString* rep,
                                 StringBuffer& sb) {
  MOZ_ASSERT(str->length() > 0);

  AutoCheckCannotGC nogc;
  const StrChar* strChars = str->chars<StrChar>(nogc);
  const RepChar* repChars = rep->chars<RepChar>(nogc);

  // The pattern is empty, so we interleave the replacement string in-between
  // each character.
  if (!pat->length()) {
    CheckedInt<uint32_t> strLength(str->length());
    CheckedInt<uint32_t> repLength(rep->length());
    CheckedInt<uint32_t> length = repLength * (strLength - 1) + strLength;
    if (!length.isValid()) {
      ReportAllocationOverflow(cx);
      return false;
    }

    if (!sb.reserve(length.value())) {
      return false;
    }

    for (unsigned i = 0; i < str->length() - 1; ++i, ++strChars) {
      sb.infallibleAppend(*strChars);
      sb.infallibleAppend(repChars, rep->length());
    }
    sb.infallibleAppend(*strChars);
    return true;
  }

  // If it's true, we are sure that the result's length is, at least, the same
  // length as |str->length()|.
  if (rep->length() >= pat->length()) {
    if (!sb.reserve(str->length())) {
      return false;
    }
  }

  uint32_t start = 0;
  for (;;) {
    int match = StringMatch(str, pat, start);
    if (match < 0) {
      break;
    }
    if (!sb.append(strChars + start, match - start)) {
      return false;
    }
    if (!sb.append(repChars, rep->length())) {
      return false;
    }
    start = match + pat->length();
  }

  if (!sb.append(strChars + start, str->length() - start)) {
    return false;
  }

  return true;
}

// This is identical to "str.split(pattern).join(replacement)" except that we
// do some deforestation optimization in Ion.
JSString* js::StringFlatReplaceString(JSContext* cx, HandleString string,
                                      HandleString pattern,
                                      HandleString replacement) {
  MOZ_ASSERT(string);
  MOZ_ASSERT(pattern);
  MOZ_ASSERT(replacement);

  if (!string->length()) {
    return string;
  }

  Rooted<JSLinearString*> linearRepl(cx, replacement->ensureLinear(cx));
  if (!linearRepl) {
    return nullptr;
  }

  Rooted<JSLinearString*> linearPat(cx, pattern->ensureLinear(cx));
  if (!linearPat) {
    return nullptr;
  }

  Rooted<JSLinearString*> linearStr(cx, string->ensureLinear(cx));
  if (!linearStr) {
    return nullptr;
  }

  JSStringBuilder sb(cx);
  if (linearStr->hasTwoByteChars()) {
    if (!sb.ensureTwoByteChars()) {
      return nullptr;
    }
    if (linearRepl->hasTwoByteChars()) {
      if (!StrFlatReplaceGlobal<char16_t, char16_t>(cx, linearStr, linearPat,
                                                    linearRepl, sb)) {
        return nullptr;
      }
    } else {
      if (!StrFlatReplaceGlobal<char16_t, Latin1Char>(cx, linearStr, linearPat,
                                                      linearRepl, sb)) {
        return nullptr;
      }
    }
  } else {
    if (linearRepl->hasTwoByteChars()) {
      if (!sb.ensureTwoByteChars()) {
        return nullptr;
      }
      if (!StrFlatReplaceGlobal<Latin1Char, char16_t>(cx, linearStr, linearPat,
                                                      linearRepl, sb)) {
        return nullptr;
      }
    } else {
      if (!StrFlatReplaceGlobal<Latin1Char, Latin1Char>(
              cx, linearStr, linearPat, linearRepl, sb)) {
        return nullptr;
      }
    }
  }

  return sb.finishString();
}

JSString* js::str_replace_string_raw(JSContext* cx, HandleString string,
                                     HandleString pattern,
                                     HandleString replacement) {
  Rooted<JSLinearString*> repl(cx, replacement->ensureLinear(cx));
  if (!repl) {
    return nullptr;
  }

  Rooted<JSLinearString*> pat(cx, pattern->ensureLinear(cx));
  if (!pat) {
    return nullptr;
  }

  size_t patternLength = pat->length();
  int32_t match;
  uint32_t dollarIndex;

  {
    AutoCheckCannotGC nogc;
    dollarIndex =
        repl->hasLatin1Chars()
            ? FindDollarIndex(repl->latin1Chars(nogc), repl->length())
            : FindDollarIndex(repl->twoByteChars(nogc), repl->length());
  }

  /*
   * |string| could be a rope, so we want to avoid flattening it for as
   * long as possible.
   */
  if (string->isRope()) {
    if (!RopeMatch(cx, &string->asRope(), pat, &match)) {
      return nullptr;
    }
  } else {
    match = StringMatch(&string->asLinear(), pat, 0);
  }

  if (match < 0) {
    return string;
  }

  if (dollarIndex != UINT32_MAX) {
    repl = InterpretDollarReplacement(cx, string, repl, dollarIndex, match,
                                      patternLength);
    if (!repl) {
      return nullptr;
    }
  } else if (string->isRope()) {
    return BuildFlatRopeReplacement(cx, string, repl, match, patternLength);
  }
  return BuildFlatReplacement(cx, string, repl, match, patternLength);
}

template <typename StrChar, typename RepChar>
static bool ReplaceAllInternal(const AutoCheckCannotGC& nogc,
                               JSLinearString* string,
                               JSLinearString* searchString,
                               JSLinearString* replaceString,
                               const int32_t startPosition,
                               JSStringBuilder& result) {
  // Step 7.
  const size_t stringLength = string->length();
  const size_t searchLength = searchString->length();
  const size_t replaceLength = replaceString->length();

  MOZ_ASSERT(stringLength > 0);
  MOZ_ASSERT(searchLength > 0);
  MOZ_ASSERT(stringLength >= searchLength);

  // Step 12.
  uint32_t endOfLastMatch = 0;

  const StrChar* strChars = string->chars<StrChar>(nogc);
  const RepChar* repChars = replaceString->chars<RepChar>(nogc);

  uint32_t dollarIndex = FindDollarIndex(repChars, replaceLength);

  // If it's true, we are sure that the result's length is, at least, the same
  // length as |str->length()|.
  if (replaceLength >= searchLength) {
    if (!result.reserve(stringLength)) {
      return false;
    }
  }

  int32_t position = startPosition;
  do {
    // Step 14.c.
    // Append the substring before the current match.
    if (!result.append(strChars + endOfLastMatch, position - endOfLastMatch)) {
      return false;
    }

    // Steps 14.a-b and 14.d.
    // Append the replacement.
    if (dollarIndex != UINT32_MAX) {
      size_t matchLimit = position + searchLength;
      if (!AppendDollarReplacement(result, dollarIndex, position, matchLimit,
                                   string, repChars, replaceLength)) {
        return false;
      }
    } else {
      if (!result.append(repChars, replaceLength)) {
        return false;
      }
    }

    // Step 14.e.
    endOfLastMatch = position + searchLength;

    // Step 11.
    // Find the next match.
    position = StringMatch(string, searchString, endOfLastMatch);
  } while (position >= 0);

  // Step 15.
  // Append the substring after the last match.
  return result.append(strChars + endOfLastMatch,
                       stringLength - endOfLastMatch);
}

// https://tc39.es/proposal-string-replaceall/#sec-string.prototype.replaceall
// Steps 7-16 when functionalReplace is false and searchString is not empty.
//
// The steps are quite different, for performance. Loops in steps 11 and 14
// are fused. GetSubstitution is optimized away when possible.
template <typename StrChar, typename RepChar>
static JSString* ReplaceAll(JSContext* cx, JSLinearString* string,
                            JSLinearString* searchString,
                            JSLinearString* replaceString) {
  // Step 7 moved into ReplaceAll_internal.

  // Step 8 (advanceBy is equal to searchLength when searchLength > 0).

  // Step 9 (not needed in this implementation).

  // Step 10.
  // Find the first match.
  int32_t position = StringMatch(string, searchString, 0);

  // Nothing to replace, so return early.
  if (position < 0) {
    return string;
  }

  // Steps 11, 12 moved into ReplaceAll_internal.

  // Step 13.
  JSStringBuilder result(cx);
  if constexpr (std::is_same_v<StrChar, char16_t> ||
                std::is_same_v<RepChar, char16_t>) {
    if (!result.ensureTwoByteChars()) {
      return nullptr;
    }
  }

  bool internalFailure = false;
  {
    AutoCheckCannotGC nogc;
    internalFailure = !ReplaceAllInternal<StrChar, RepChar>(
        nogc, string, searchString, replaceString, position, result);
  }
  if (internalFailure) {
    return nullptr;
  }

  // Step 16.
  return result.finishString();
}

template <typename StrChar, typename RepChar>
static bool ReplaceAllInterleaveInternal(const AutoCheckCannotGC& nogc,
                                         JSContext* cx, JSLinearString* string,
                                         JSLinearString* replaceString,
                                         JSStringBuilder& result) {
  // Step 7.
  const size_t stringLength = string->length();
  const size_t replaceLength = replaceString->length();

  const StrChar* strChars = string->chars<StrChar>(nogc);
  const RepChar* repChars = replaceString->chars<RepChar>(nogc);

  uint32_t dollarIndex = FindDollarIndex(repChars, replaceLength);

  if (dollarIndex != UINT32_MAX) {
    if (!result.reserve(stringLength)) {
      return false;
    }
  } else {
    // Compute the exact result length when no substitutions take place.
    CheckedInt<uint32_t> strLength(stringLength);
    CheckedInt<uint32_t> repLength(replaceLength);
    CheckedInt<uint32_t> length = strLength + (strLength + 1) * repLength;
    if (!length.isValid()) {
      ReportAllocationOverflow(cx);
      return false;
    }

    if (!result.reserve(length.value())) {
      return false;
    }
  }

  auto appendReplacement = [&](size_t match) {
    if (dollarIndex != UINT32_MAX) {
      return AppendDollarReplacement(result, dollarIndex, match, match, string,
                                     repChars, replaceLength);
    }
    return result.append(repChars, replaceLength);
  };

  for (size_t index = 0; index < stringLength; index++) {
    // Steps 11, 14.a-b and 14.d.
    // The empty string matches before each character.
    if (!appendReplacement(index)) {
      return false;
    }

    // Step 14.c.
    if (!result.append(strChars[index])) {
      return false;
    }
  }

  // Steps 11, 14.a-b and 14.d.
  // The empty string also matches at the end of the string.
  return appendReplacement(stringLength);

  // Step 15 (not applicable when searchString is the empty string).
}

// https://tc39.es/proposal-string-replaceall/#sec-string.prototype.replaceall
// Steps 7-16 when functionalReplace is false and searchString is the empty
// string.
//
// The steps are quite different, for performance. Loops in steps 11 and 14
// are fused. GetSubstitution is optimized away when possible.
template <typename StrChar, typename RepChar>
static JSString* ReplaceAllInterleave(JSContext* cx, JSLinearString* string,
                                      JSLinearString* replaceString) {
  // Step 7 moved into ReplaceAllInterleavedInternal.

  // Step 8 (advanceBy is 1 when searchString is the empty string).

  // Steps 9-12 (trivial when searchString is the empty string).

  // Step 13.
  JSStringBuilder result(cx);
  if constexpr (std::is_same_v<StrChar, char16_t> ||
                std::is_same_v<RepChar, char16_t>) {
    if (!result.ensureTwoByteChars()) {
      return nullptr;
    }
  }

  bool internalFailure = false;
  {
    AutoCheckCannotGC nogc;
    internalFailure = !ReplaceAllInterleaveInternal<StrChar, RepChar>(
        nogc, cx, string, replaceString, result);
  }
  if (internalFailure) {
    return nullptr;
  }

  // Step 16.
  return result.finishString();
}

// String.prototype.replaceAll (Stage 3 proposal)
// https://tc39.es/proposal-string-replaceall/
//
// String.prototype.replaceAll ( searchValue, replaceValue )
//
// Steps 7-16 when functionalReplace is false.
JSString* js::str_replaceAll_string_raw(JSContext* cx, HandleString string,
                                        HandleString searchString,
                                        HandleString replaceString) {
  const size_t stringLength = string->length();
  const size_t searchLength = searchString->length();

  // Directly return when we're guaranteed to find no match.
  if (searchLength > stringLength) {
    return string;
  }

  Rooted<JSLinearString*> str(cx, string->ensureLinear(cx));
  if (!str) {
    return nullptr;
  }

  Rooted<JSLinearString*> repl(cx, replaceString->ensureLinear(cx));
  if (!repl) {
    return nullptr;
  }

  Rooted<JSLinearString*> search(cx, searchString->ensureLinear(cx));
  if (!search) {
    return nullptr;
  }

  // The pattern is empty, so we interleave the replacement string in-between
  // each character.
  if (searchLength == 0) {
    if (str->hasTwoByteChars()) {
      if (repl->hasTwoByteChars()) {
        return ReplaceAllInterleave<char16_t, char16_t>(cx, str, repl);
      }
      return ReplaceAllInterleave<char16_t, Latin1Char>(cx, str, repl);
    }
    if (repl->hasTwoByteChars()) {
      return ReplaceAllInterleave<Latin1Char, char16_t>(cx, str, repl);
    }
    return ReplaceAllInterleave<Latin1Char, Latin1Char>(cx, str, repl);
  }

  MOZ_ASSERT(stringLength > 0);

  if (str->hasTwoByteChars()) {
    if (repl->hasTwoByteChars()) {
      return ReplaceAll<char16_t, char16_t>(cx, str, search, repl);
    }
    return ReplaceAll<char16_t, Latin1Char>(cx, str, search, repl);
  }
  if (repl->hasTwoByteChars()) {
    return ReplaceAll<Latin1Char, char16_t>(cx, str, search, repl);
  }
  return ReplaceAll<Latin1Char, Latin1Char>(cx, str, search, repl);
}

static ArrayObject* SingleElementStringArray(JSContext* cx,
                                             Handle<JSLinearString*> str) {
  ArrayObject* array = NewDenseFullyAllocatedArray(cx, 1);
  if (!array) {
    return nullptr;
  }
  array->setDenseInitializedLength(1);
  array->initDenseElement(0, StringValue(str));
  return array;
}

// ES 2016 draft Mar 25, 2016 21.1.3.17 steps 4, 8, 12-18.
static ArrayObject* SplitHelper(JSContext* cx, Handle<JSLinearString*> str,
                                uint32_t limit, Handle<JSLinearString*> sep) {
  size_t strLength = str->length();
  size_t sepLength = sep->length();
  MOZ_ASSERT(sepLength != 0);

  // Step 12.
  if (strLength == 0) {
    // Step 12.a.
    int match = StringMatch(str, sep, 0);

    // Step 12.b.
    if (match != -1) {
      return NewDenseEmptyArray(cx);
    }

    // Steps 12.c-e.
    return SingleElementStringArray(cx, str);
  }

  // Step 3 (reordered).
  RootedValueVector splits(cx);

  // Step 8 (reordered).
  size_t lastEndIndex = 0;

  // Step 13.
  size_t index = 0;

  // Step 14.
  while (index != strLength) {
    // Step 14.a.
    int match = StringMatch(str, sep, index);

    // Step 14.b.
    //
    // Our match algorithm differs from the spec in that it returns the
    // next index at which a match happens.  If no match happens we're
    // done.
    //
    // But what if the match is at the end of the string (and the string is
    // not empty)?  Per 14.c.i this shouldn't be a match, so we have to
    // specially exclude it.  Thus this case should hold:
    //
    //   var a = "abc".split(/\b/);
    //   assertEq(a.length, 1);
    //   assertEq(a[0], "abc");
    if (match == -1) {
      break;
    }

    // Step 14.c.
    size_t endIndex = match + sepLength;

    // Step 14.c.i.
    if (endIndex == lastEndIndex) {
      index++;
      continue;
    }

    // Step 14.c.ii.
    MOZ_ASSERT(lastEndIndex < endIndex);
    MOZ_ASSERT(sepLength <= strLength);
    MOZ_ASSERT(lastEndIndex + sepLength <= endIndex);

    // Step 14.c.ii.1.
    size_t subLength = size_t(endIndex - sepLength - lastEndIndex);
    JSString* sub = NewDependentString(cx, str, lastEndIndex, subLength);

    // Steps 14.c.ii.2-4.
    if (!sub || !splits.append(StringValue(sub))) {
      return nullptr;
    }

    // Step 14.c.ii.5.
    if (splits.length() == limit) {
      return NewDenseCopiedArray(cx, splits.length(), splits.begin());
    }

    // Step 14.c.ii.6.
    index = endIndex;

    // Step 14.c.ii.7.
    lastEndIndex = index;
  }

  // Step 15.
  JSString* sub =
      NewDependentString(cx, str, lastEndIndex, strLength - lastEndIndex);

  // Steps 16-17.
  if (!sub || !splits.append(StringValue(sub))) {
    return nullptr;
  }

  // Step 18.
  return NewDenseCopiedArray(cx, splits.length(), splits.begin());
}

// Fast-path for splitting a string into a character array via split("").
static ArrayObject* CharSplitHelper(JSContext* cx, Handle<JSLinearString*> str,
                                    uint32_t limit) {
  size_t strLength = str->length();
  if (strLength == 0) {
    return NewDenseEmptyArray(cx);
  }

  js::StaticStrings& staticStrings = cx->staticStrings();
  uint32_t resultlen = (limit < strLength ? limit : strLength);
  MOZ_ASSERT(limit > 0 && resultlen > 0,
             "Neither limit nor strLength is zero, so resultlen is greater "
             "than zero.");

  Rooted<ArrayObject*> splits(cx, NewDenseFullyAllocatedArray(cx, resultlen));
  if (!splits) {
    return nullptr;
  }

  if (str->hasLatin1Chars()) {
    splits->setDenseInitializedLength(resultlen);

    JS::AutoCheckCannotGC nogc;
    const Latin1Char* latin1Chars = str->latin1Chars(nogc);
    for (size_t i = 0; i < resultlen; ++i) {
      Latin1Char c = latin1Chars[i];
      MOZ_ASSERT(staticStrings.hasUnit(c));
      splits->initDenseElement(i, StringValue(staticStrings.getUnit(c)));
    }
  } else {
    splits->ensureDenseInitializedLength(0, resultlen);

    for (size_t i = 0; i < resultlen; ++i) {
      JSString* sub = staticStrings.getUnitStringForElement(cx, str, i);
      if (!sub) {
        return nullptr;
      }
      splits->initDenseElement(i, StringValue(sub));
    }
  }

  return splits;
}

template <typename TextChar>
static MOZ_ALWAYS_INLINE ArrayObject* SplitSingleCharHelper(
    JSContext* cx, Handle<JSLinearString*> str, const TextChar* text,
    uint32_t textLen, char16_t patCh) {
  // Count the number of occurrences of patCh within text.
  uint32_t count = 0;
  for (size_t index = 0; index < textLen; index++) {
    if (static_cast<char16_t>(text[index]) == patCh) {
      count++;
    }
  }

  // Handle zero-occurrence case - return input string in an array.
  if (count == 0) {
    return SingleElementStringArray(cx, str);
  }

  // Create the result array for the substring values.
  Rooted<ArrayObject*> splits(cx, NewDenseFullyAllocatedArray(cx, count + 1));
  if (!splits) {
    return nullptr;
  }
  splits->ensureDenseInitializedLength(0, count + 1);

  // Add substrings.
  uint32_t splitsIndex = 0;
  size_t lastEndIndex = 0;
  for (size_t index = 0; index < textLen; index++) {
    if (static_cast<char16_t>(text[index]) == patCh) {
      size_t subLength = size_t(index - lastEndIndex);
      JSString* sub = NewDependentString(cx, str, lastEndIndex, subLength);
      if (!sub) {
        return nullptr;
      }
      splits->initDenseElement(splitsIndex++, StringValue(sub));
      lastEndIndex = index + 1;
    }
  }

  // Add substring for tail of string (after last match).
  JSString* sub =
      NewDependentString(cx, str, lastEndIndex, textLen - lastEndIndex);
  if (!sub) {
    return nullptr;
  }
  splits->initDenseElement(splitsIndex++, StringValue(sub));

  return splits;
}

// ES 2016 draft Mar 25, 2016 21.1.3.17 steps 4, 8, 12-18.
static ArrayObject* SplitSingleCharHelper(JSContext* cx,
                                          Handle<JSLinearString*> str,
                                          char16_t ch) {
  // Step 12.
  size_t strLength = str->length();

  AutoStableStringChars linearChars(cx);
  if (!linearChars.init(cx, str)) {
    return nullptr;
  }

  if (linearChars.isLatin1()) {
    return SplitSingleCharHelper(cx, str, linearChars.latin1Chars(), strLength,
                                 ch);
  }

  return SplitSingleCharHelper(cx, str, linearChars.twoByteChars(), strLength,
                               ch);
}

// ES 2016 draft Mar 25, 2016 21.1.3.17 steps 4, 8, 12-18.
ArrayObject* js::StringSplitString(JSContext* cx, HandleString str,
                                   HandleString sep, uint32_t limit) {
  MOZ_ASSERT(limit > 0, "Only called for strictly positive limit.");

  Rooted<JSLinearString*> linearStr(cx, str->ensureLinear(cx));
  if (!linearStr) {
    return nullptr;
  }

  Rooted<JSLinearString*> linearSep(cx, sep->ensureLinear(cx));
  if (!linearSep) {
    return nullptr;
  }

  if (linearSep->length() == 0) {
    return CharSplitHelper(cx, linearStr, limit);
  }

  if (linearSep->length() == 1 && limit >= static_cast<uint32_t>(INT32_MAX)) {
    char16_t ch = linearSep->latin1OrTwoByteChar(0);
    return SplitSingleCharHelper(cx, linearStr, ch);
  }

  return SplitHelper(cx, linearStr, limit, linearSep);
}

static const JSFunctionSpec string_methods[] = {
    JS_FN(js_toSource_str, str_toSource, 0, 0),

    /* Java-like methods. */
    JS_INLINABLE_FN(js_toString_str, str_toString, 0, 0, StringToString),
    JS_INLINABLE_FN(js_valueOf_str, str_toString, 0, 0, StringValueOf),
    JS_INLINABLE_FN("toLowerCase", str_toLowerCase, 0, 0, StringToLowerCase),
    JS_INLINABLE_FN("toUpperCase", str_toUpperCase, 0, 0, StringToUpperCase),
    JS_INLINABLE_FN("charAt", str_charAt, 1, 0, StringCharAt),
    JS_INLINABLE_FN("charCodeAt", str_charCodeAt, 1, 0, StringCharCodeAt),
    JS_SELF_HOSTED_FN("substring", "String_substring", 2, 0),
    JS_SELF_HOSTED_FN("padStart", "String_pad_start", 2, 0),
    JS_SELF_HOSTED_FN("padEnd", "String_pad_end", 2, 0),
    JS_SELF_HOSTED_FN("codePointAt", "String_codePointAt", 1, 0),
    JS_FN("includes", str_includes, 1, 0),
    JS_INLINABLE_FN("indexOf", str_indexOf, 1, 0, StringIndexOf),
    JS_FN("lastIndexOf", str_lastIndexOf, 1, 0),
    JS_INLINABLE_FN("startsWith", str_startsWith, 1, 0, StringStartsWith),
    JS_INLINABLE_FN("endsWith", str_endsWith, 1, 0, StringEndsWith),
    JS_FN("trim", str_trim, 0, 0),
    JS_FN("trimStart", str_trimStart, 0, 0),
    JS_FN("trimEnd", str_trimEnd, 0, 0),
#if JS_HAS_INTL_API
    JS_SELF_HOSTED_FN("toLocaleLowerCase", "String_toLocaleLowerCase", 0, 0),
    JS_SELF_HOSTED_FN("toLocaleUpperCase", "String_toLocaleUpperCase", 0, 0),
    JS_SELF_HOSTED_FN("localeCompare", "String_localeCompare", 1, 0),
#else
    JS_FN("toLocaleLowerCase", str_toLocaleLowerCase, 0, 0),
    JS_FN("toLocaleUpperCase", str_toLocaleUpperCase, 0, 0),
    JS_FN("localeCompare", str_localeCompare, 1, 0),
#endif
    JS_SELF_HOSTED_FN("repeat", "String_repeat", 1, 0),
#if JS_HAS_INTL_API
    JS_FN("normalize", str_normalize, 0, 0),
#endif
#ifdef NIGHTLY_BUILD
    JS_FN("isWellFormed", str_isWellFormed, 0, 0),
    JS_FN("toWellFormed", str_toWellFormed, 0, 0),
#endif

    /* Perl-ish methods (search is actually Python-esque). */
    JS_SELF_HOSTED_FN("match", "String_match", 1, 0),
    JS_SELF_HOSTED_FN("matchAll", "String_matchAll", 1, 0),
    JS_SELF_HOSTED_FN("search", "String_search", 1, 0),
    JS_SELF_HOSTED_FN("replace", "String_replace", 2, 0),
    JS_SELF_HOSTED_FN("replaceAll", "String_replaceAll", 2, 0),
    JS_SELF_HOSTED_FN("split", "String_split", 2, 0),
    JS_SELF_HOSTED_FN("substr", "String_substr", 2, 0),

    /* Python-esque sequence methods. */
    JS_SELF_HOSTED_FN("concat", "String_concat", 1, 0),
    JS_SELF_HOSTED_FN("slice", "String_slice", 2, 0),

    JS_SELF_HOSTED_FN("at", "String_at", 1, 0),

    /* HTML string methods. */
    JS_SELF_HOSTED_FN("bold", "String_bold", 0, 0),
    JS_SELF_HOSTED_FN("italics", "String_italics", 0, 0),
    JS_SELF_HOSTED_FN("fixed", "String_fixed", 0, 0),
    JS_SELF_HOSTED_FN("strike", "String_strike", 0, 0),
    JS_SELF_HOSTED_FN("small", "String_small", 0, 0),
    JS_SELF_HOSTED_FN("big", "String_big", 0, 0),
    JS_SELF_HOSTED_FN("blink", "String_blink", 0, 0),
    JS_SELF_HOSTED_FN("sup", "String_sup", 0, 0),
    JS_SELF_HOSTED_FN("sub", "String_sub", 0, 0),
    JS_SELF_HOSTED_FN("anchor", "String_anchor", 1, 0),
    JS_SELF_HOSTED_FN("link", "String_link", 1, 0),
    JS_SELF_HOSTED_FN("fontcolor", "String_fontcolor", 1, 0),
    JS_SELF_HOSTED_FN("fontsize", "String_fontsize", 1, 0),

    JS_SELF_HOSTED_SYM_FN(iterator, "String_iterator", 0, 0),
    JS_FS_END,
};

// ES6 rev 27 (2014 Aug 24) 21.1.1
bool js::StringConstructor(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedString str(cx);
  if (args.length() > 0) {
    if (!args.isConstructing() && args[0].isSymbol()) {
      return js::SymbolDescriptiveString(cx, args[0].toSymbol(), args.rval());
    }

    str = ToString<CanGC>(cx, args[0]);
    if (!str) {
      return false;
    }
  } else {
    str = cx->runtime()->emptyString;
  }

  if (args.isConstructing()) {
    RootedObject proto(cx);
    if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_String, &proto)) {
      return false;
    }

    StringObject* strobj = StringObject::create(cx, str, proto);
    if (!strobj) {
      return false;
    }
    args.rval().setObject(*strobj);
    return true;
  }

  args.rval().setString(str);
  return true;
}

bool js::str_fromCharCode(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  MOZ_ASSERT(args.length() <= ARGS_LENGTH_MAX);

  // Optimize the single-char case.
  if (args.length() == 1) {
    return str_fromCharCode_one_arg(cx, args[0], args.rval());
  }

  // Optimize the case where the result will definitely fit in an inline
  // string (thin or fat) and so we don't need to malloc the chars. (We could
  // cover some cases where args.length() goes up to
  // JSFatInlineString::MAX_LENGTH_LATIN1 if we also checked if the chars are
  // all Latin-1, but it doesn't seem worth the effort.)
  InlineCharBuffer<char16_t> chars;
  if (!chars.maybeAlloc(cx, args.length())) {
    return false;
  }

  char16_t* rawChars = chars.get();
  for (unsigned i = 0; i < args.length(); i++) {
    uint16_t code;
    if (!ToUint16(cx, args[i], &code)) {
      return false;
    }

    rawChars[i] = char16_t(code);
  }

  JSString* str = chars.toString(cx, args.length());
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

static inline bool CodeUnitToString(JSContext* cx, uint16_t ucode,
                                    MutableHandleValue rval) {
  if (StaticStrings::hasUnit(ucode)) {
    rval.setString(cx->staticStrings().getUnit(ucode));
    return true;
  }

  char16_t c = char16_t(ucode);
  JSString* str = NewStringCopyNDontDeflate<CanGC>(cx, &c, 1);
  if (!str) {
    return false;
  }

  rval.setString(str);
  return true;
}

bool js::str_fromCharCode_one_arg(JSContext* cx, HandleValue code,
                                  MutableHandleValue rval) {
  uint16_t ucode;

  if (!ToUint16(cx, code, &ucode)) {
    return false;
  }

  return CodeUnitToString(cx, ucode, rval);
}

static MOZ_ALWAYS_INLINE bool ToCodePoint(JSContext* cx, HandleValue code,
                                          char32_t* codePoint) {
  // String.fromCodePoint, Steps 5.a-b.

  // Fast path for the common case - the input is already an int32.
  if (code.isInt32()) {
    int32_t nextCP = code.toInt32();
    if (nextCP >= 0 && nextCP <= int32_t(unicode::NonBMPMax)) {
      *codePoint = char32_t(nextCP);
      return true;
    }
  }

  double nextCP;
  if (!ToNumber(cx, code, &nextCP)) {
    return false;
  }

  // String.fromCodePoint, Steps 5.c-d.
  if (JS::ToInteger(nextCP) != nextCP || nextCP < 0 ||
      nextCP > unicode::NonBMPMax) {
    ToCStringBuf cbuf;
    const char* numStr = NumberToCString(&cbuf, nextCP);
    MOZ_ASSERT(numStr);
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_A_CODEPOINT, numStr);
    return false;
  }

  *codePoint = char32_t(nextCP);
  return true;
}

bool js::str_fromCodePoint_one_arg(JSContext* cx, HandleValue code,
                                   MutableHandleValue rval) {
  // Steps 1-4 (omitted).

  // Steps 5.a-d.
  char32_t codePoint;
  if (!ToCodePoint(cx, code, &codePoint)) {
    return false;
  }

  // Steps 5.e, 6.
  if (!unicode::IsSupplementary(codePoint)) {
    return CodeUnitToString(cx, uint16_t(codePoint), rval);
  }

  char16_t chars[] = {unicode::LeadSurrogate(codePoint),
                      unicode::TrailSurrogate(codePoint)};
  JSString* str = NewStringCopyNDontDeflate<CanGC>(cx, chars, 2);
  if (!str) {
    return false;
  }

  rval.setString(str);
  return true;
}

static bool str_fromCodePoint_few_args(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(args.length() <= JSFatInlineString::MAX_LENGTH_TWO_BYTE / 2);

  // Steps 1-2 (omitted).

  // Step 3.
  char16_t elements[JSFatInlineString::MAX_LENGTH_TWO_BYTE];

  // Steps 4-5.
  unsigned length = 0;
  for (unsigned nextIndex = 0; nextIndex < args.length(); nextIndex++) {
    // Steps 5.a-d.
    char32_t codePoint;
    if (!ToCodePoint(cx, args[nextIndex], &codePoint)) {
      return false;
    }

    // Step 5.e.
    unicode::UTF16Encode(codePoint, elements, &length);
  }

  // Step 6.
  JSString* str = NewStringCopyN<CanGC>(cx, elements, length);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

// ES2017 draft rev 40edb3a95a475c1b251141ac681b8793129d9a6d
// 21.1.2.2 String.fromCodePoint(...codePoints)
bool js::str_fromCodePoint(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Optimize the single code-point case.
  if (args.length() == 1) {
    return str_fromCodePoint_one_arg(cx, args[0], args.rval());
  }

  // Optimize the case where the result will definitely fit in an inline
  // string (thin or fat) and so we don't need to malloc the chars. (We could
  // cover some cases where |args.length()| goes up to
  // JSFatInlineString::MAX_LENGTH_LATIN1 / 2 if we also checked if the chars
  // are all Latin-1, but it doesn't seem worth the effort.)
  if (args.length() <= JSFatInlineString::MAX_LENGTH_TWO_BYTE / 2) {
    return str_fromCodePoint_few_args(cx, args);
  }

  // Steps 1-2 (omitted).

  // Step 3.
  static_assert(
      ARGS_LENGTH_MAX < std::numeric_limits<decltype(args.length())>::max() / 2,
      "|args.length() * 2| does not overflow");
  auto elements = cx->make_pod_arena_array<char16_t>(js::StringBufferArena,
                                                     args.length() * 2);
  if (!elements) {
    return false;
  }

  // Steps 4-5.
  unsigned length = 0;
  for (unsigned nextIndex = 0; nextIndex < args.length(); nextIndex++) {
    // Steps 5.a-d.
    char32_t codePoint;
    if (!ToCodePoint(cx, args[nextIndex], &codePoint)) {
      return false;
    }

    // Step 5.e.
    unicode::UTF16Encode(codePoint, elements.get(), &length);
  }

  // Step 6.
  JSString* str = NewString<CanGC>(cx, std::move(elements), length);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

static const JSFunctionSpec string_static_methods[] = {
    JS_INLINABLE_FN("fromCharCode", js::str_fromCharCode, 1, 0,
                    StringFromCharCode),
    JS_INLINABLE_FN("fromCodePoint", js::str_fromCodePoint, 1, 0,
                    StringFromCodePoint),

    JS_SELF_HOSTED_FN("raw", "String_static_raw", 1, 0), JS_FS_END};

/* static */
SharedShape* StringObject::assignInitialShape(JSContext* cx,
                                              Handle<StringObject*> obj) {
  MOZ_ASSERT(obj->empty());

  if (!NativeObject::addPropertyInReservedSlot(cx, obj, cx->names().length,
                                               LENGTH_SLOT, {})) {
    return nullptr;
  }

  return obj->sharedShape();
}

JSObject* StringObject::createPrototype(JSContext* cx, JSProtoKey key) {
  Rooted<JSString*> empty(cx, cx->runtime()->emptyString);
  Rooted<StringObject*> proto(
      cx, GlobalObject::createBlankPrototype<StringObject>(cx, cx->global()));
  if (!proto) {
    return nullptr;
  }
  if (!StringObject::init(cx, proto, empty)) {
    return nullptr;
  }
  return proto;
}

static bool StringClassFinish(JSContext* cx, HandleObject ctor,
                              HandleObject proto) {
  Handle<NativeObject*> nativeProto = proto.as<NativeObject>();

  // Create "trimLeft" as an alias for "trimStart".
  RootedValue trimFn(cx);
  RootedId trimId(cx, NameToId(cx->names().trimStart));
  RootedId trimAliasId(cx, NameToId(cx->names().trimLeft));
  if (!NativeGetProperty(cx, nativeProto, trimId, &trimFn) ||
      !NativeDefineDataProperty(cx, nativeProto, trimAliasId, trimFn, 0)) {
    return false;
  }

  // Create "trimRight" as an alias for "trimEnd".
  trimId = NameToId(cx->names().trimEnd);
  trimAliasId = NameToId(cx->names().trimRight);
  if (!NativeGetProperty(cx, nativeProto, trimId, &trimFn) ||
      !NativeDefineDataProperty(cx, nativeProto, trimAliasId, trimFn, 0)) {
    return false;
  }

  /*
   * Define escape/unescape, the URI encode/decode functions, and maybe
   * uneval on the global object.
   */
  if (!JS_DefineFunctions(cx, cx->global(), string_functions)) {
    return false;
  }

  return true;
}

const ClassSpec StringObject::classSpec_ = {
    GenericCreateConstructor<StringConstructor, 1, gc::AllocKind::FUNCTION,
                             &jit::JitInfo_String>,
    StringObject::createPrototype,
    string_static_methods,
    nullptr,
    string_methods,
    nullptr,
    StringClassFinish};

#define ____ false

/*
 * Uri reserved chars + #:
 * - 35: #
 * - 36: $
 * - 38: &
 * - 43: +
 * - 44: ,
 * - 47: /
 * - 58: :
 * - 59: ;
 * - 61: =
 * - 63: ?
 * - 64: @
 */
static const bool js_isUriReservedPlusPound[] = {
    // clang-format off
/*       0     1     2     3     4     5     6     7     8     9  */
/*  0 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  1 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  2 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  3 */ ____, ____, ____, ____, ____, true, true, ____, true, ____,
/*  4 */ ____, ____, ____, true, true, ____, ____, true, ____, ____,
/*  5 */ ____, ____, ____, ____, ____, ____, ____, ____, true, true,
/*  6 */ ____, true, ____, true, true, ____, ____, ____, ____, ____,
/*  7 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  8 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  9 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/* 10 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/* 11 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/* 12 */ ____, ____, ____, ____, ____, ____, ____, ____
    // clang-format on
};

/*
 * Uri unescaped chars:
 * -      33: !
 * -      39: '
 * -      40: (
 * -      41: )
 * -      42: *
 * -      45: -
 * -      46: .
 * -  48..57: 0-9
 * -  65..90: A-Z
 * -      95: _
 * - 97..122: a-z
 * -     126: ~
 */
static const bool js_isUriUnescaped[] = {
    // clang-format off
/*       0     1     2     3     4     5     6     7     8     9  */
/*  0 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  1 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  2 */ ____, ____, ____, ____, ____, ____, ____, ____, ____, ____,
/*  3 */ ____, ____, ____, true, ____, ____, ____, ____, ____, true,
/*  4 */ true, true, true, ____, ____, true, true, ____, true, true,
/*  5 */ true, true, true, true, true, true, true, true, ____, ____,
/*  6 */ ____, ____, ____, ____, ____, true, true, true, true, true,
/*  7 */ true, true, true, true, true, true, true, true, true, true,
/*  8 */ true, true, true, true, true, true, true, true, true, true,
/*  9 */ true, ____, ____, ____, ____, true, ____, true, true, true,
/* 10 */ true, true, true, true, true, true, true, true, true, true,
/* 11 */ true, true, true, true, true, true, true, true, true, true,
/* 12 */ true, true, true, ____, ____, ____, true, ____
    // clang-format on
};

#undef ____

static inline bool TransferBufferToString(JSStringBuilder& sb, JSString* str,
                                          MutableHandleValue rval) {
  if (!sb.empty()) {
    str = sb.finishString();
    if (!str) {
      return false;
    }
  }
  rval.setString(str);
  return true;
}

/*
 * ECMA 3, 15.1.3 URI Handling Function Properties
 *
 * The following are implementations of the algorithms
 * given in the ECMA specification for the hidden functions
 * 'Encode' and 'Decode'.
 */
enum EncodeResult { Encode_Failure, Encode_BadUri, Encode_Success };

// Bug 1403318: GCC sometimes inlines this Encode function rather than the
// caller Encode function. Annotate both functions with MOZ_NEVER_INLINE resp.
// MOZ_ALWAYS_INLINE to ensure we get the desired inlining behavior.
template <typename CharT>
static MOZ_NEVER_INLINE EncodeResult Encode(StringBuffer& sb,
                                            const CharT* chars, size_t length,
                                            const bool* unescapedSet) {
  Latin1Char hexBuf[3];
  hexBuf[0] = '%';

  auto appendEncoded = [&sb, &hexBuf](Latin1Char c) {
    static const char HexDigits[] = "0123456789ABCDEF"; /* NB: uppercase */

    hexBuf[1] = HexDigits[c >> 4];
    hexBuf[2] = HexDigits[c & 0xf];
    return sb.append(hexBuf, 3);
  };

  auto appendRange = [&sb, chars, length](size_t start, size_t end) {
    MOZ_ASSERT(start <= end);

    if (start < end) {
      if (start == 0) {
        if (!sb.reserve(length)) {
          return false;
        }
      }
      return sb.append(chars + start, chars + end);
    }
    return true;
  };

  size_t startAppend = 0;
  for (size_t k = 0; k < length; k++) {
    CharT c = chars[k];
    if (c < 128 &&
        (js_isUriUnescaped[c] || (unescapedSet && unescapedSet[c]))) {
      continue;
    } else {
      if (!appendRange(startAppend, k)) {
        return Encode_Failure;
      }

      if constexpr (std::is_same_v<CharT, Latin1Char>) {
        if (c < 0x80) {
          if (!appendEncoded(c)) {
            return Encode_Failure;
          }
        } else {
          if (!appendEncoded(0xC0 | (c >> 6)) ||
              !appendEncoded(0x80 | (c & 0x3F))) {
            return Encode_Failure;
          }
        }
      } else {
        if (unicode::IsTrailSurrogate(c)) {
          return Encode_BadUri;
        }

        char32_t v;
        if (!unicode::IsLeadSurrogate(c)) {
          v = c;
        } else {
          k++;
          if (k == length) {
            return Encode_BadUri;
          }

          char16_t c2 = chars[k];
          if (!unicode::IsTrailSurrogate(c2)) {
            return Encode_BadUri;
          }

          v = unicode::UTF16Decode(c, c2);
        }

        uint8_t utf8buf[4];
        size_t L = OneUcs4ToUtf8Char(utf8buf, v);
        for (size_t j = 0; j < L; j++) {
          if (!appendEncoded(utf8buf[j])) {
            return Encode_Failure;
          }
        }
      }

      startAppend = k + 1;
    }
  }

  if (startAppend > 0) {
    if (!appendRange(startAppend, length)) {
      return Encode_Failure;
    }
  }

  return Encode_Success;
}

static MOZ_ALWAYS_INLINE bool Encode(JSContext* cx, Handle<JSLinearString*> str,
                                     const bool* unescapedSet,
                                     MutableHandleValue rval) {
  size_t length = str->length();
  if (length == 0) {
    rval.setString(cx->runtime()->emptyString);
    return true;
  }

  JSStringBuilder sb(cx);

  EncodeResult res;
  if (str->hasLatin1Chars()) {
    AutoCheckCannotGC nogc;
    res = Encode(sb, str->latin1Chars(nogc), str->length(), unescapedSet);
  } else {
    AutoCheckCannotGC nogc;
    res = Encode(sb, str->twoByteChars(nogc), str->length(), unescapedSet);
  }

  if (res == Encode_Failure) {
    return false;
  }

  if (res == Encode_BadUri) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_URI);
    return false;
  }

  MOZ_ASSERT(res == Encode_Success);
  return TransferBufferToString(sb, str, rval);
}

enum DecodeResult { Decode_Failure, Decode_BadUri, Decode_Success };

template <typename CharT>
static DecodeResult Decode(StringBuffer& sb, const CharT* chars, size_t length,
                           const bool* reservedSet) {
  auto appendRange = [&sb, chars](size_t start, size_t end) {
    MOZ_ASSERT(start <= end);

    if (start < end) {
      return sb.append(chars + start, chars + end);
    }
    return true;
  };

  size_t startAppend = 0;
  for (size_t k = 0; k < length; k++) {
    CharT c = chars[k];
    if (c == '%') {
      size_t start = k;
      if ((k + 2) >= length) {
        return Decode_BadUri;
      }

      if (!IsAsciiHexDigit(chars[k + 1]) || !IsAsciiHexDigit(chars[k + 2])) {
        return Decode_BadUri;
      }

      uint32_t B = AsciiAlphanumericToNumber(chars[k + 1]) * 16 +
                   AsciiAlphanumericToNumber(chars[k + 2]);
      k += 2;
      if (B < 128) {
        Latin1Char ch = Latin1Char(B);
        if (reservedSet && reservedSet[ch]) {
          continue;
        }

        if (!appendRange(startAppend, start)) {
          return Decode_Failure;
        }
        if (!sb.append(ch)) {
          return Decode_Failure;
        }
      } else {
        int n = 1;
        while (B & (0x80 >> n)) {
          n++;
        }

        if (n == 1 || n > 4) {
          return Decode_BadUri;
        }

        uint8_t octets[4];
        octets[0] = (uint8_t)B;
        if (k + 3 * (n - 1) >= length) {
          return Decode_BadUri;
        }

        for (int j = 1; j < n; j++) {
          k++;
          if (chars[k] != '%') {
            return Decode_BadUri;
          }

          if (!IsAsciiHexDigit(chars[k + 1]) ||
              !IsAsciiHexDigit(chars[k + 2])) {
            return Decode_BadUri;
          }

          B = AsciiAlphanumericToNumber(chars[k + 1]) * 16 +
              AsciiAlphanumericToNumber(chars[k + 2]);
          if ((B & 0xC0) != 0x80) {
            return Decode_BadUri;
          }

          k += 2;
          octets[j] = char(B);
        }

        if (!appendRange(startAppend, start)) {
          return Decode_Failure;
        }

        char32_t v = JS::Utf8ToOneUcs4Char(octets, n);
        MOZ_ASSERT(v >= 128);
        if (v >= unicode::NonBMPMin) {
          if (v > unicode::NonBMPMax) {
            return Decode_BadUri;
          }

          if (!sb.append(unicode::LeadSurrogate(v))) {
            return Decode_Failure;
          }
          if (!sb.append(unicode::TrailSurrogate(v))) {
            return Decode_Failure;
          }
        } else {
          if (!sb.append(char16_t(v))) {
            return Decode_Failure;
          }
        }
      }

      startAppend = k + 1;
    }
  }

  if (startAppend > 0) {
    if (!appendRange(startAppend, length)) {
      return Decode_Failure;
    }
  }

  return Decode_Success;
}

static bool Decode(JSContext* cx, Handle<JSLinearString*> str,
                   const bool* reservedSet, MutableHandleValue rval) {
  size_t length = str->length();
  if (length == 0) {
    rval.setString(cx->runtime()->emptyString);
    return true;
  }

  JSStringBuilder sb(cx);

  DecodeResult res;
  if (str->hasLatin1Chars()) {
    AutoCheckCannotGC nogc;
    res = Decode(sb, str->latin1Chars(nogc), str->length(), reservedSet);
  } else {
    AutoCheckCannotGC nogc;
    res = Decode(sb, str->twoByteChars(nogc), str->length(), reservedSet);
  }

  if (res == Decode_Failure) {
    return false;
  }

  if (res == Decode_BadUri) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_URI);
    return false;
  }

  MOZ_ASSERT(res == Decode_Success);
  return TransferBufferToString(sb, str, rval);
}

static bool str_decodeURI(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "decodeURI");
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<JSLinearString*> str(cx, ArgToLinearString(cx, args, 0));
  if (!str) {
    return false;
  }

  return Decode(cx, str, js_isUriReservedPlusPound, args.rval());
}

static bool str_decodeURI_Component(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "decodeURIComponent");
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<JSLinearString*> str(cx, ArgToLinearString(cx, args, 0));
  if (!str) {
    return false;
  }

  return Decode(cx, str, nullptr, args.rval());
}

static bool str_encodeURI(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "encodeURI");
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<JSLinearString*> str(cx, ArgToLinearString(cx, args, 0));
  if (!str) {
    return false;
  }

  return Encode(cx, str, js_isUriReservedPlusPound, args.rval());
}

static bool str_encodeURI_Component(JSContext* cx, unsigned argc, Value* vp) {
  AutoJSMethodProfilerEntry pseudoFrame(cx, "encodeURIComponent");
  CallArgs args = CallArgsFromVp(argc, vp);
  Rooted<JSLinearString*> str(cx, ArgToLinearString(cx, args, 0));
  if (!str) {
    return false;
  }

  return Encode(cx, str, nullptr, args.rval());
}

JSString* js::EncodeURI(JSContext* cx, const char* chars, size_t length) {
  JSStringBuilder sb(cx);
  EncodeResult result = Encode(sb, reinterpret_cast<const Latin1Char*>(chars),
                               length, js_isUriReservedPlusPound);
  if (result == EncodeResult::Encode_Failure) {
    return nullptr;
  }
  if (result == EncodeResult::Encode_BadUri) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_BAD_URI);
    return nullptr;
  }
  if (sb.empty()) {
    return NewStringCopyN<CanGC>(cx, chars, length);
  }
  return sb.finishString();
}

static bool FlatStringMatchHelper(JSContext* cx, HandleString str,
                                  HandleString pattern, bool* isFlat,
                                  int32_t* match) {
  Rooted<JSLinearString*> linearPattern(cx, pattern->ensureLinear(cx));
  if (!linearPattern) {
    return false;
  }

  static const size_t MAX_FLAT_PAT_LEN = 256;
  if (linearPattern->length() > MAX_FLAT_PAT_LEN ||
      StringHasRegExpMetaChars(linearPattern)) {
    *isFlat = false;
    return true;
  }

  *isFlat = true;
  if (str->isRope()) {
    if (!RopeMatch(cx, &str->asRope(), linearPattern, match)) {
      return false;
    }
  } else {
    *match = StringMatch(&str->asLinear(), linearPattern);
  }

  return true;
}

static bool BuildFlatMatchArray(JSContext* cx, HandleString str,
                                HandleString pattern, int32_t match,
                                MutableHandleValue rval) {
  if (match < 0) {
    rval.setNull();
    return true;
  }

  // Get the templateObject that defines the shape and type of the output
  // object.
  ArrayObject* templateObject =
      cx->realm()->regExps.getOrCreateMatchResultTemplateObject(cx);
  if (!templateObject) {
    return false;
  }

  Rooted<ArrayObject*> arr(
      cx, NewDenseFullyAllocatedArrayWithTemplate(cx, 1, templateObject));
  if (!arr) {
    return false;
  }

  // Store a Value for each pair.
  arr->setDenseInitializedLength(1);
  arr->initDenseElement(0, StringValue(pattern));

  // Set the |index| property. (TemplateObject positions it in slot 0).
  arr->setSlot(0, Int32Value(match));

  // Set the |input| property. (TemplateObject positions it in slot 1).
  arr->setSlot(1, StringValue(str));

#ifdef DEBUG
  RootedValue test(cx);
  RootedId id(cx, NameToId(cx->names().index));
  if (!NativeGetProperty(cx, arr, id, &test)) {
    return false;
  }
  MOZ_ASSERT(test == arr->getSlot(0));
  id = NameToId(cx->names().input);
  if (!NativeGetProperty(cx, arr, id, &test)) {
    return false;
  }
  MOZ_ASSERT(test == arr->getSlot(1));
#endif

  rval.setObject(*arr);
  return true;
}

#ifdef DEBUG
static bool CallIsStringOptimizable(JSContext* cx, const char* name,
                                    bool* result) {
  FixedInvokeArgs<0> args(cx);

  RootedValue rval(cx);
  if (!CallSelfHostedFunction(cx, name, UndefinedHandleValue, args, &rval)) {
    return false;
  }

  *result = rval.toBoolean();
  return true;
}
#endif

bool js::FlatStringMatch(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(args[0].isString());
  MOZ_ASSERT(args[1].isString());
#ifdef DEBUG
  bool isOptimizable = false;
  if (!CallIsStringOptimizable(cx, "IsStringMatchOptimizable",
                               &isOptimizable)) {
    return false;
  }
  MOZ_ASSERT(isOptimizable);
#endif

  RootedString str(cx, args[0].toString());
  RootedString pattern(cx, args[1].toString());

  bool isFlat = false;
  int32_t match = 0;
  if (!FlatStringMatchHelper(cx, str, pattern, &isFlat, &match)) {
    return false;
  }

  if (!isFlat) {
    args.rval().setUndefined();
    return true;
  }

  return BuildFlatMatchArray(cx, str, pattern, match, args.rval());
}

bool js::FlatStringSearch(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);
  MOZ_ASSERT(args[0].isString());
  MOZ_ASSERT(args[1].isString());
#ifdef DEBUG
  bool isOptimizable = false;
  if (!CallIsStringOptimizable(cx, "IsStringSearchOptimizable",
                               &isOptimizable)) {
    return false;
  }
  MOZ_ASSERT(isOptimizable);
#endif

  RootedString str(cx, args[0].toString());
  RootedString pattern(cx, args[1].toString());

  bool isFlat = false;
  int32_t match = 0;
  if (!FlatStringMatchHelper(cx, str, pattern, &isFlat, &match)) {
    return false;
  }

  if (!isFlat) {
    args.rval().setInt32(-2);
    return true;
  }

  args.rval().setInt32(match);
  return true;
}
