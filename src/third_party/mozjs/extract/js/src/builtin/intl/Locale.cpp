/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Intl.Locale implementation. */

#include "builtin/intl/Locale.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/intl/Locale.h"
#include "mozilla/Maybe.h"
#include "mozilla/Span.h"
#include "mozilla/TextUtils.h"

#include <algorithm>
#include <string>
#include <string.h>
#include <utility>

#include "builtin/Boolean.h"
#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/FormatBuffer.h"
#include "builtin/intl/LanguageTag.h"
#include "builtin/intl/StringAsciiChars.h"
#include "builtin/String.h"
#include "js/Conversions.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/Printer.h"
#include "js/TypeDecls.h"
#include "js/Wrapper.h"
#include "vm/Compartment.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/StringType.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using namespace mozilla::intl::LanguageTagLimits;

const JSClass LocaleObject::class_ = {
    "Intl.Locale",
    JSCLASS_HAS_RESERVED_SLOTS(LocaleObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_Locale),
    JS_NULL_CLASS_OPS, &LocaleObject::classSpec_};

const JSClass& LocaleObject::protoClass_ = PlainObject::class_;

static inline bool IsLocale(HandleValue v) {
  return v.isObject() && v.toObject().is<LocaleObject>();
}

// Return the length of the base-name subtags.
static size_t BaseNameLength(const mozilla::intl::Locale& tag) {
  size_t baseNameLength = tag.Language().Length();
  if (tag.Script().Present()) {
    baseNameLength += 1 + tag.Script().Length();
  }
  if (tag.Region().Present()) {
    baseNameLength += 1 + tag.Region().Length();
  }
  for (const auto& variant : tag.Variants()) {
    baseNameLength += 1 + variant.size();
  }
  return baseNameLength;
}

struct IndexAndLength {
  size_t index;
  size_t length;

  IndexAndLength(size_t index, size_t length) : index(index), length(length){};

  template <typename T>
  mozilla::Span<const T> spanOf(const T* ptr) const {
    return {ptr + index, length};
  }
};

// Compute the Unicode extension's index and length in the extension subtag.
static mozilla::Maybe<IndexAndLength> UnicodeExtensionPosition(
    const mozilla::intl::Locale& tag) {
  size_t index = 0;
  for (const auto& extension : tag.Extensions()) {
    MOZ_ASSERT(!mozilla::IsAsciiUppercaseAlpha(extension[0]),
               "extensions are case normalized to lowercase");

    size_t extensionLength = extension.size();
    if (extension[0] == 'u') {
      return mozilla::Some(IndexAndLength{index, extensionLength});
    }

    // Add +1 to skip over the preceding separator.
    index += 1 + extensionLength;
  }
  return mozilla::Nothing();
}

static LocaleObject* CreateLocaleObject(JSContext* cx, HandleObject prototype,
                                        const mozilla::intl::Locale& tag) {
  intl::FormatBuffer<char, intl::INITIAL_CHAR_BUFFER_SIZE> buffer(cx);
  if (auto result = tag.ToString(buffer); result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return nullptr;
  }

  RootedString tagStr(cx, buffer.toAsciiString(cx));
  if (!tagStr) {
    return nullptr;
  }

  size_t baseNameLength = BaseNameLength(tag);

  RootedString baseName(cx, NewDependentString(cx, tagStr, 0, baseNameLength));
  if (!baseName) {
    return nullptr;
  }

  RootedValue unicodeExtension(cx, UndefinedValue());
  if (auto result = UnicodeExtensionPosition(tag)) {
    JSString* str = NewDependentString(
        cx, tagStr, baseNameLength + 1 + result->index, result->length);
    if (!str) {
      return nullptr;
    }

    unicodeExtension.setString(str);
  }

  auto* locale = NewObjectWithClassProto<LocaleObject>(cx, prototype);
  if (!locale) {
    return nullptr;
  }

  locale->setFixedSlot(LocaleObject::LANGUAGE_TAG_SLOT, StringValue(tagStr));
  locale->setFixedSlot(LocaleObject::BASENAME_SLOT, StringValue(baseName));
  locale->setFixedSlot(LocaleObject::UNICODE_EXTENSION_SLOT, unicodeExtension);

  return locale;
}

static inline bool IsValidUnicodeExtensionValue(JSContext* cx,
                                                JSLinearString* linear,
                                                bool* isValid) {
  if (linear->length() == 0) {
    *isValid = false;
    return true;
  }

  if (!StringIsAscii(linear)) {
    *isValid = false;
    return true;
  }

  intl::StringAsciiChars chars(linear);
  if (!chars.init(cx)) {
    return false;
  }

  *isValid =
      mozilla::intl::LocaleParser::CanParseUnicodeExtensionType(chars).isOk();
  return true;
}

/** Iterate through (sep keyword) in a valid, lowercased Unicode extension. */
template <typename CharT>
class SepKeywordIterator {
  const CharT* iter_;
  const CharT* const end_;

 public:
  SepKeywordIterator(const CharT* unicodeExtensionBegin,
                     const CharT* unicodeExtensionEnd)
      : iter_(unicodeExtensionBegin), end_(unicodeExtensionEnd) {}

  /**
   * Return (sep keyword) in the Unicode locale extension from begin to end.
   * The first call after all (sep keyword) are consumed returns |nullptr|; no
   * further calls are allowed.
   */
  const CharT* next() {
    MOZ_ASSERT(iter_ != nullptr,
               "can't call next() once it's returned nullptr");

    constexpr size_t SepKeyLength = 1 + UnicodeKeyLength;  // "-co"/"-nu"/etc.

    MOZ_ASSERT(iter_ + SepKeyLength <= end_,
               "overall Unicode locale extension or non-leading subtags must "
               "be at least key-sized");

    MOZ_ASSERT((iter_[0] == 'u' && iter_[1] == '-') || iter_[0] == '-');

    while (true) {
      // Skip past '-' so |std::char_traits::find| makes progress. Skipping
      // 'u' is harmless -- skip or not, |find| returns the first '-'.
      iter_++;

      // Find the next separator.
      iter_ = std::char_traits<CharT>::find(
          iter_, mozilla::PointerRangeSize(iter_, end_), CharT('-'));
      if (!iter_) {
        return nullptr;
      }

      MOZ_ASSERT(iter_ + SepKeyLength <= end_,
                 "non-leading subtags in a Unicode locale extension are all "
                 "at least as long as a key");

      if (iter_ + SepKeyLength == end_ ||  // key is terminal subtag
          iter_[SepKeyLength] == '-') {    // key is followed by more subtags
        break;
      }
    }

    MOZ_ASSERT(iter_[0] == '-');
    MOZ_ASSERT(mozilla::IsAsciiLowercaseAlpha(iter_[1]) ||
               mozilla::IsAsciiDigit(iter_[1]));
    MOZ_ASSERT(mozilla::IsAsciiLowercaseAlpha(iter_[2]));
    MOZ_ASSERT_IF(iter_ + SepKeyLength < end_, iter_[SepKeyLength] == '-');
    return iter_;
  }
};

/**
 * 9.2.10 GetOption ( options, property, type, values, fallback )
 *
 * If the requested property is present and not-undefined, set the result string
 * to |ToString(value)|. Otherwise set the result string to nullptr.
 */
static bool GetStringOption(JSContext* cx, HandleObject options,
                            Handle<PropertyName*> name,
                            MutableHandle<JSLinearString*> string) {
  // Step 1.
  RootedValue option(cx);
  if (!GetProperty(cx, options, options, name, &option)) {
    return false;
  }

  // Step 2.
  JSLinearString* linear = nullptr;
  if (!option.isUndefined()) {
    // Steps 2.a-b, 2.d (not applicable).

    // Steps 2.c, 2.e.
    JSString* str = ToString(cx, option);
    if (!str) {
      return false;
    }
    linear = str->ensureLinear(cx);
    if (!linear) {
      return false;
    }
  }

  // Step 3.
  string.set(linear);
  return true;
}

/**
 * 9.2.10 GetOption ( options, property, type, values, fallback )
 *
 * If the requested property is present and not-undefined, set the result string
 * to |ToString(ToBoolean(value))|. Otherwise set the result string to nullptr.
 */
static bool GetBooleanOption(JSContext* cx, HandleObject options,
                             Handle<PropertyName*> name,
                             MutableHandle<JSLinearString*> string) {
  // Step 1.
  RootedValue option(cx);
  if (!GetProperty(cx, options, options, name, &option)) {
    return false;
  }

  // Step 2.
  JSLinearString* linear = nullptr;
  if (!option.isUndefined()) {
    // Steps 2.a, 2.c-d (not applicable).

    // Steps 2.c, 2.e.
    linear = BooleanToString(cx, ToBoolean(option));
  }

  // Step 3.
  string.set(linear);
  return true;
}

/**
 * ApplyOptionsToTag ( tag, options )
 */
static bool ApplyOptionsToTag(JSContext* cx, mozilla::intl::Locale& tag,
                              HandleObject options) {
  // Steps 1-2 (Already performed in caller).

  Rooted<JSLinearString*> option(cx);

  // Step 3.
  if (!GetStringOption(cx, options, cx->names().language, &option)) {
    return false;
  }

  // Step 4.
  mozilla::intl::LanguageSubtag language;
  if (option && !intl::ParseStandaloneLanguageTag(option, language)) {
    if (UniqueChars str = QuoteString(cx, option, '"')) {
      JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                                JSMSG_INVALID_OPTION_VALUE, "language",
                                str.get());
    }
    return false;
  }

  // Step 5.
  if (!GetStringOption(cx, options, cx->names().script, &option)) {
    return false;
  }

  // Step 6.
  mozilla::intl::ScriptSubtag script;
  if (option && !intl::ParseStandaloneScriptTag(option, script)) {
    if (UniqueChars str = QuoteString(cx, option, '"')) {
      JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                                JSMSG_INVALID_OPTION_VALUE, "script",
                                str.get());
    }
    return false;
  }

  // Step 7.
  if (!GetStringOption(cx, options, cx->names().region, &option)) {
    return false;
  }

  // Step 8.
  mozilla::intl::RegionSubtag region;
  if (option && !intl::ParseStandaloneRegionTag(option, region)) {
    if (UniqueChars str = QuoteString(cx, option, '"')) {
      JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                                JSMSG_INVALID_OPTION_VALUE, "region",
                                str.get());
    }
    return false;
  }

  // Step 9 (Already performed in caller).

  // Skip steps 10-13 when no subtags were modified.
  if (language.Present() || script.Present() || region.Present()) {
    // Step 10.
    if (language.Present()) {
      tag.SetLanguage(language);
    }

    // Step 11.
    if (script.Present()) {
      tag.SetScript(script);
    }

    // Step 12.
    if (region.Present()) {
      tag.SetRegion(region);
    }

    // Step 13.
    // Optimized to only canonicalize the base-name subtags. All other
    // canonicalization steps will happen later.
    auto result = tag.CanonicalizeBaseName();
    if (result.isErr()) {
      if (result.unwrapErr() ==
          mozilla::intl::Locale::CanonicalizationError::DuplicateVariant) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                  JSMSG_DUPLICATE_VARIANT_SUBTAG);
      } else {
        intl::ReportInternalError(cx);
      }
      return false;
    }
  }

  return true;
}

/**
 * ApplyUnicodeExtensionToTag( tag, options, relevantExtensionKeys )
 */
bool js::intl::ApplyUnicodeExtensionToTag(
    JSContext* cx, mozilla::intl::Locale& tag,
    JS::HandleVector<intl::UnicodeExtensionKeyword> keywords) {
  // If no Unicode extensions were present in the options object, we can skip
  // everything below and directly return.
  if (keywords.length() == 0) {
    return true;
  }

  Vector<char, 32> newExtension(cx);
  if (!newExtension.append('u')) {
    return false;
  }

  // Check if there's an existing Unicode extension subtag.

  const char* unicodeExtensionEnd = nullptr;
  const char* unicodeExtensionKeywords = nullptr;
  if (auto unicodeExtension = tag.GetUnicodeExtension()) {
    const char* unicodeExtensionBegin = unicodeExtension->data();
    unicodeExtensionEnd = unicodeExtensionBegin + unicodeExtension->size();

    SepKeywordIterator<char> iter(unicodeExtensionBegin, unicodeExtensionEnd);

    // Find the start of the first keyword.
    unicodeExtensionKeywords = iter.next();

    // Copy any attributes present before the first keyword.
    const char* attributesEnd = unicodeExtensionKeywords
                                    ? unicodeExtensionKeywords
                                    : unicodeExtensionEnd;
    if (!newExtension.append(unicodeExtensionBegin + 1, attributesEnd)) {
      return false;
    }
  }

  // Append the new keywords before any existing keywords. That way any previous
  // keyword with the same key is detected as a duplicate when canonicalizing
  // the Unicode extension subtag and gets discarded.

  for (const auto& keyword : keywords) {
    UnicodeExtensionKeyword::UnicodeKeySpan key = keyword.key();
    if (!newExtension.append('-')) {
      return false;
    }
    if (!newExtension.append(key.data(), key.size())) {
      return false;
    }
    if (!newExtension.append('-')) {
      return false;
    }

    JS::AutoCheckCannotGC nogc;
    JSLinearString* type = keyword.type();
    if (type->hasLatin1Chars()) {
      if (!newExtension.append(type->latin1Chars(nogc), type->length())) {
        return false;
      }
    } else {
      if (!newExtension.append(type->twoByteChars(nogc), type->length())) {
        return false;
      }
    }
  }

  // Append the remaining keywords from the previous Unicode extension subtag.
  if (unicodeExtensionKeywords) {
    if (!newExtension.append(unicodeExtensionKeywords, unicodeExtensionEnd)) {
      return false;
    }
  }

  if (auto res = tag.SetUnicodeExtension(newExtension); res.isErr()) {
    intl::ReportInternalError(cx, res.unwrapErr());
    return false;
  }

  return true;
}

static JS::Result<JSString*> LanguageTagFromMaybeWrappedLocale(JSContext* cx,
                                                               JSObject* obj) {
  if (obj->is<LocaleObject>()) {
    return obj->as<LocaleObject>().languageTag();
  }

  JSObject* unwrapped = CheckedUnwrapStatic(obj);
  if (!unwrapped) {
    ReportAccessDenied(cx);
    return cx->alreadyReportedError();
  }

  if (!unwrapped->is<LocaleObject>()) {
    return nullptr;
  }

  RootedString tagStr(cx, unwrapped->as<LocaleObject>().languageTag());
  if (!cx->compartment()->wrap(cx, &tagStr)) {
    return cx->alreadyReportedError();
  }
  return tagStr.get();
}

/**
 * Intl.Locale( tag[, options] )
 */
static bool Locale(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Step 1.
  if (!ThrowIfNotConstructing(cx, args, "Intl.Locale")) {
    return false;
  }

  // Steps 2-6 (Inlined 9.1.14, OrdinaryCreateFromConstructor).
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, JSProto_Locale, &proto)) {
    return false;
  }

  // Steps 7-9.
  HandleValue tagValue = args.get(0);
  JSString* tagStr;
  if (tagValue.isObject()) {
    JS_TRY_VAR_OR_RETURN_FALSE(
        cx, tagStr,
        LanguageTagFromMaybeWrappedLocale(cx, &tagValue.toObject()));
    if (!tagStr) {
      tagStr = ToString(cx, tagValue);
      if (!tagStr) {
        return false;
      }
    }
  } else if (tagValue.isString()) {
    tagStr = tagValue.toString();
  } else {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_INVALID_LOCALES_ELEMENT);
    return false;
  }

  Rooted<JSLinearString*> tagLinearStr(cx, tagStr->ensureLinear(cx));
  if (!tagLinearStr) {
    return false;
  }

  // Steps 10-11.
  RootedObject options(cx);
  if (args.hasDefined(1)) {
    options = ToObject(cx, args[1]);
    if (!options) {
      return false;
    }
  }

  // ApplyOptionsToTag, steps 2 and 9.
  mozilla::intl::Locale tag;
  if (!intl::ParseLocale(cx, tagLinearStr, tag)) {
    return false;
  }

  if (auto result = tag.CanonicalizeBaseName(); result.isErr()) {
    if (result.unwrapErr() ==
        mozilla::intl::Locale::CanonicalizationError::DuplicateVariant) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DUPLICATE_VARIANT_SUBTAG);
    } else {
      intl::ReportInternalError(cx);
    }
    return false;
  }

  if (options) {
    // Step 12.
    if (!ApplyOptionsToTag(cx, tag, options)) {
      return false;
    }

    // Step 13.
    JS::RootedVector<intl::UnicodeExtensionKeyword> keywords(cx);

    // Step 14.
    Rooted<JSLinearString*> calendar(cx);
    if (!GetStringOption(cx, options, cx->names().calendar, &calendar)) {
      return false;
    }

    // Steps 15-16.
    if (calendar) {
      bool isValid;
      if (!IsValidUnicodeExtensionValue(cx, calendar, &isValid)) {
        return false;
      }

      if (!isValid) {
        if (UniqueChars str = QuoteString(cx, calendar, '"')) {
          JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                                    JSMSG_INVALID_OPTION_VALUE, "calendar",
                                    str.get());
        }
        return false;
      }

      if (!keywords.emplaceBack("ca", calendar)) {
        return false;
      }
    }

    // Step 17.
    Rooted<JSLinearString*> collation(cx);
    if (!GetStringOption(cx, options, cx->names().collation, &collation)) {
      return false;
    }

    // Steps 18-19.
    if (collation) {
      bool isValid;
      if (!IsValidUnicodeExtensionValue(cx, collation, &isValid)) {
        return false;
      }

      if (!isValid) {
        if (UniqueChars str = QuoteString(cx, collation, '"')) {
          JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                                    JSMSG_INVALID_OPTION_VALUE, "collation",
                                    str.get());
        }
        return false;
      }

      if (!keywords.emplaceBack("co", collation)) {
        return false;
      }
    }

    // Step 20 (without validation).
    Rooted<JSLinearString*> hourCycle(cx);
    if (!GetStringOption(cx, options, cx->names().hourCycle, &hourCycle)) {
      return false;
    }

    // Steps 20-21.
    if (hourCycle) {
      if (!StringEqualsLiteral(hourCycle, "h11") &&
          !StringEqualsLiteral(hourCycle, "h12") &&
          !StringEqualsLiteral(hourCycle, "h23") &&
          !StringEqualsLiteral(hourCycle, "h24")) {
        if (UniqueChars str = QuoteString(cx, hourCycle, '"')) {
          JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                                    JSMSG_INVALID_OPTION_VALUE, "hourCycle",
                                    str.get());
        }
        return false;
      }

      if (!keywords.emplaceBack("hc", hourCycle)) {
        return false;
      }
    }

    // Step 22 (without validation).
    Rooted<JSLinearString*> caseFirst(cx);
    if (!GetStringOption(cx, options, cx->names().caseFirst, &caseFirst)) {
      return false;
    }

    // Steps 22-23.
    if (caseFirst) {
      if (!StringEqualsLiteral(caseFirst, "upper") &&
          !StringEqualsLiteral(caseFirst, "lower") &&
          !StringEqualsLiteral(caseFirst, "false")) {
        if (UniqueChars str = QuoteString(cx, caseFirst, '"')) {
          JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                                    JSMSG_INVALID_OPTION_VALUE, "caseFirst",
                                    str.get());
        }
        return false;
      }

      if (!keywords.emplaceBack("kf", caseFirst)) {
        return false;
      }
    }

    // Steps 24-25.
    Rooted<JSLinearString*> numeric(cx);
    if (!GetBooleanOption(cx, options, cx->names().numeric, &numeric)) {
      return false;
    }

    // Step 26.
    if (numeric) {
      if (!keywords.emplaceBack("kn", numeric)) {
        return false;
      }
    }

    // Step 27.
    Rooted<JSLinearString*> numberingSystem(cx);
    if (!GetStringOption(cx, options, cx->names().numberingSystem,
                         &numberingSystem)) {
      return false;
    }

    // Steps 28-29.
    if (numberingSystem) {
      bool isValid;
      if (!IsValidUnicodeExtensionValue(cx, numberingSystem, &isValid)) {
        return false;
      }
      if (!isValid) {
        if (UniqueChars str = QuoteString(cx, numberingSystem, '"')) {
          JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                                    JSMSG_INVALID_OPTION_VALUE,
                                    "numberingSystem", str.get());
        }
        return false;
      }

      if (!keywords.emplaceBack("nu", numberingSystem)) {
        return false;
      }
    }

    // Step 30.
    if (!ApplyUnicodeExtensionToTag(cx, tag, keywords)) {
      return false;
    }
  }

  // ApplyOptionsToTag, steps 9 and 13.
  // ApplyUnicodeExtensionToTag, step 9.
  if (auto result = tag.CanonicalizeExtensions(); result.isErr()) {
    if (result.unwrapErr() ==
        mozilla::intl::Locale::CanonicalizationError::DuplicateVariant) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DUPLICATE_VARIANT_SUBTAG);
    } else {
      intl::ReportInternalError(cx);
    }
    return false;
  }

  // Steps 6, 31-37.
  JSObject* obj = CreateLocaleObject(cx, proto, tag);
  if (!obj) {
    return false;
  }

  // Step 38.
  args.rval().setObject(*obj);
  return true;
}

using UnicodeKey = const char (&)[UnicodeKeyLength + 1];

// Returns the tuple [index, length] of the `type` in the `keyword` in Unicode
// locale extension |extension| that has |key| as its `key`. If `keyword` lacks
// a type, the returned |index| will be where `type` would have been, and
// |length| will be set to zero.
template <typename CharT>
static mozilla::Maybe<IndexAndLength> FindUnicodeExtensionType(
    const CharT* extension, size_t length, UnicodeKey key) {
  MOZ_ASSERT(extension[0] == 'u');
  MOZ_ASSERT(extension[1] == '-');

  const CharT* end = extension + length;

  SepKeywordIterator<CharT> iter(extension, end);

  // Search all keywords until a match was found.
  const CharT* beginKey;
  while (true) {
    beginKey = iter.next();
    if (!beginKey) {
      return mozilla::Nothing();
    }

    // Add +1 to skip over the separator preceding the keyword.
    MOZ_ASSERT(beginKey[0] == '-');
    beginKey++;

    // Exit the loop on the first match.
    if (std::equal(beginKey, beginKey + UnicodeKeyLength, key)) {
      break;
    }
  }

  // Skip over the key.
  const CharT* beginType = beginKey + UnicodeKeyLength;

  // Find the start of the next keyword.
  const CharT* endType = iter.next();

  // No further keyword present, the current keyword ends the Unicode extension.
  if (!endType) {
    endType = end;
  }

  // If the keyword has a type, skip over the separator preceding the type.
  if (beginType != endType) {
    MOZ_ASSERT(beginType[0] == '-');
    beginType++;
  }
  return mozilla::Some(IndexAndLength{size_t(beginType - extension),
                                      size_t(endType - beginType)});
}

static inline auto FindUnicodeExtensionType(JSLinearString* unicodeExtension,
                                            UnicodeKey key) {
  JS::AutoCheckCannotGC nogc;
  return unicodeExtension->hasLatin1Chars()
             ? FindUnicodeExtensionType(
                   reinterpret_cast<const char*>(
                       unicodeExtension->latin1Chars(nogc)),
                   unicodeExtension->length(), key)
             : FindUnicodeExtensionType(unicodeExtension->twoByteChars(nogc),
                                        unicodeExtension->length(), key);
}

// Return the sequence of types for the Unicode extension keyword specified by
// key or undefined when the keyword isn't present.
static bool GetUnicodeExtension(JSContext* cx, LocaleObject* locale,
                                UnicodeKey key, MutableHandleValue value) {
  // Return undefined when no Unicode extension subtag is present.
  const Value& unicodeExtensionValue = locale->unicodeExtension();
  if (unicodeExtensionValue.isUndefined()) {
    value.setUndefined();
    return true;
  }

  JSLinearString* unicodeExtension =
      unicodeExtensionValue.toString()->ensureLinear(cx);
  if (!unicodeExtension) {
    return false;
  }

  // Find the type of the requested key in the Unicode extension subtag.
  auto result = FindUnicodeExtensionType(unicodeExtension, key);

  // Return undefined if the requested key isn't present in the extension.
  if (!result) {
    value.setUndefined();
    return true;
  }

  size_t index = result->index;
  size_t length = result->length;

  // Otherwise return the type value of the found keyword.
  JSString* str = NewDependentString(cx, unicodeExtension, index, length);
  if (!str) {
    return false;
  }
  value.setString(str);
  return true;
}

struct BaseNamePartsResult {
  IndexAndLength language;
  mozilla::Maybe<IndexAndLength> script;
  mozilla::Maybe<IndexAndLength> region;
};

// Returns [language-length, script-index, region-index, region-length].
template <typename CharT>
static BaseNamePartsResult BaseNameParts(const CharT* baseName, size_t length) {
  size_t languageLength;
  size_t scriptIndex = 0;
  size_t regionIndex = 0;
  size_t regionLength = 0;

  // Search the first separator to find the end of the language subtag.
  if (const CharT* sep = std::char_traits<CharT>::find(baseName, length, '-')) {
    languageLength = sep - baseName;

    // Add +1 to skip over the separator character.
    size_t nextSubtag = languageLength + 1;

    // Script subtags are always four characters long, but take care for a four
    // character long variant subtag. These start with a digit.
    if ((nextSubtag + ScriptLength == length ||
         (nextSubtag + ScriptLength < length &&
          baseName[nextSubtag + ScriptLength] == '-')) &&
        mozilla::IsAsciiAlpha(baseName[nextSubtag])) {
      scriptIndex = nextSubtag;
      nextSubtag = scriptIndex + ScriptLength + 1;
    }

    // Region subtags can be either two or three characters long.
    if (nextSubtag < length) {
      for (size_t rlen : {AlphaRegionLength, DigitRegionLength}) {
        MOZ_ASSERT(nextSubtag + rlen <= length);
        if (nextSubtag + rlen == length || baseName[nextSubtag + rlen] == '-') {
          regionIndex = nextSubtag;
          regionLength = rlen;
          break;
        }
      }
    }
  } else {
    // No separator found, the base-name consists of just a language subtag.
    languageLength = length;
  }

  // Tell the analysis the |IsStructurallyValid*Tag| functions can't GC.
  JS::AutoSuppressGCAnalysis nogc;

  IndexAndLength language{0, languageLength};
  MOZ_ASSERT(
      mozilla::intl::IsStructurallyValidLanguageTag(language.spanOf(baseName)));

  mozilla::Maybe<IndexAndLength> script{};
  if (scriptIndex) {
    script.emplace(scriptIndex, ScriptLength);
    MOZ_ASSERT(
        mozilla::intl::IsStructurallyValidScriptTag(script->spanOf(baseName)));
  }

  mozilla::Maybe<IndexAndLength> region{};
  if (regionIndex) {
    region.emplace(regionIndex, regionLength);
    MOZ_ASSERT(
        mozilla::intl::IsStructurallyValidRegionTag(region->spanOf(baseName)));
  }

  return {language, script, region};
}

static inline auto BaseNameParts(JSLinearString* baseName) {
  JS::AutoCheckCannotGC nogc;
  return baseName->hasLatin1Chars()
             ? BaseNameParts(
                   reinterpret_cast<const char*>(baseName->latin1Chars(nogc)),
                   baseName->length())
             : BaseNameParts(baseName->twoByteChars(nogc), baseName->length());
}

// Intl.Locale.prototype.maximize ()
static bool Locale_maximize(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsLocale(args.thisv()));

  // Step 3.
  auto* locale = &args.thisv().toObject().as<LocaleObject>();
  Rooted<JSLinearString*> tagStr(cx, locale->languageTag()->ensureLinear(cx));
  if (!tagStr) {
    return false;
  }

  mozilla::intl::Locale tag;
  if (!intl::ParseLocale(cx, tagStr, tag)) {
    return false;
  }

  if (auto result = tag.AddLikelySubtags(); result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return false;
  }

  // Step 4.
  auto* result = CreateLocaleObject(cx, nullptr, tag);
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  return true;
}

// Intl.Locale.prototype.maximize ()
static bool Locale_maximize(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsLocale, Locale_maximize>(cx, args);
}

// Intl.Locale.prototype.minimize ()
static bool Locale_minimize(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsLocale(args.thisv()));

  // Step 3.
  auto* locale = &args.thisv().toObject().as<LocaleObject>();
  Rooted<JSLinearString*> tagStr(cx, locale->languageTag()->ensureLinear(cx));
  if (!tagStr) {
    return false;
  }

  mozilla::intl::Locale tag;
  if (!intl::ParseLocale(cx, tagStr, tag)) {
    return false;
  }

  if (auto result = tag.RemoveLikelySubtags(); result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return false;
  }

  // Step 4.
  auto* result = CreateLocaleObject(cx, nullptr, tag);
  if (!result) {
    return false;
  }
  args.rval().setObject(*result);
  return true;
}

// Intl.Locale.prototype.minimize ()
static bool Locale_minimize(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsLocale, Locale_minimize>(cx, args);
}

// Intl.Locale.prototype.toString ()
static bool Locale_toString(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsLocale(args.thisv()));

  // Step 3.
  auto* locale = &args.thisv().toObject().as<LocaleObject>();
  args.rval().setString(locale->languageTag());
  return true;
}

// Intl.Locale.prototype.toString ()
static bool Locale_toString(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsLocale, Locale_toString>(cx, args);
}

// get Intl.Locale.prototype.baseName
static bool Locale_baseName(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsLocale(args.thisv()));

  // Steps 3-4.
  auto* locale = &args.thisv().toObject().as<LocaleObject>();
  args.rval().setString(locale->baseName());
  return true;
}

// get Intl.Locale.prototype.baseName
static bool Locale_baseName(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsLocale, Locale_baseName>(cx, args);
}

// get Intl.Locale.prototype.calendar
static bool Locale_calendar(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsLocale(args.thisv()));

  // Step 3.
  auto* locale = &args.thisv().toObject().as<LocaleObject>();
  return GetUnicodeExtension(cx, locale, "ca", args.rval());
}

// get Intl.Locale.prototype.calendar
static bool Locale_calendar(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsLocale, Locale_calendar>(cx, args);
}

// get Intl.Locale.prototype.caseFirst
static bool Locale_caseFirst(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsLocale(args.thisv()));

  // Step 3.
  auto* locale = &args.thisv().toObject().as<LocaleObject>();
  return GetUnicodeExtension(cx, locale, "kf", args.rval());
}

// get Intl.Locale.prototype.caseFirst
static bool Locale_caseFirst(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsLocale, Locale_caseFirst>(cx, args);
}

// get Intl.Locale.prototype.collation
static bool Locale_collation(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsLocale(args.thisv()));

  // Step 3.
  auto* locale = &args.thisv().toObject().as<LocaleObject>();
  return GetUnicodeExtension(cx, locale, "co", args.rval());
}

// get Intl.Locale.prototype.collation
static bool Locale_collation(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsLocale, Locale_collation>(cx, args);
}

// get Intl.Locale.prototype.hourCycle
static bool Locale_hourCycle(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsLocale(args.thisv()));

  // Step 3.
  auto* locale = &args.thisv().toObject().as<LocaleObject>();
  return GetUnicodeExtension(cx, locale, "hc", args.rval());
}

// get Intl.Locale.prototype.hourCycle
static bool Locale_hourCycle(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsLocale, Locale_hourCycle>(cx, args);
}

// get Intl.Locale.prototype.numeric
static bool Locale_numeric(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsLocale(args.thisv()));

  // Step 3.
  auto* locale = &args.thisv().toObject().as<LocaleObject>();
  RootedValue value(cx);
  if (!GetUnicodeExtension(cx, locale, "kn", &value)) {
    return false;
  }

  // Compare against the empty string per Intl.Locale, step 36.a. The Unicode
  // extension is already canonicalized, so we don't need to compare against
  // "true" at this point.
  MOZ_ASSERT(value.isUndefined() || value.isString());
  MOZ_ASSERT_IF(value.isString(),
                !StringEqualsLiteral(&value.toString()->asLinear(), "true"));

  args.rval().setBoolean(value.isString() && value.toString()->empty());
  return true;
}

// get Intl.Locale.prototype.numeric
static bool Locale_numeric(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsLocale, Locale_numeric>(cx, args);
}

// get Intl.Locale.prototype.numberingSystem
static bool Intl_Locale_numberingSystem(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsLocale(args.thisv()));

  // Step 3.
  auto* locale = &args.thisv().toObject().as<LocaleObject>();
  return GetUnicodeExtension(cx, locale, "nu", args.rval());
}

// get Intl.Locale.prototype.numberingSystem
static bool Locale_numberingSystem(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsLocale, Intl_Locale_numberingSystem>(cx, args);
}

// get Intl.Locale.prototype.language
static bool Locale_language(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsLocale(args.thisv()));

  // Step 3.
  auto* locale = &args.thisv().toObject().as<LocaleObject>();
  JSLinearString* baseName = locale->baseName()->ensureLinear(cx);
  if (!baseName) {
    return false;
  }

  // Step 4 (Unnecessary assertion).

  auto language = BaseNameParts(baseName).language;

  size_t index = language.index;
  size_t length = language.length;

  // Step 5.
  JSString* str = NewDependentString(cx, baseName, index, length);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

// get Intl.Locale.prototype.language
static bool Locale_language(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsLocale, Locale_language>(cx, args);
}

// get Intl.Locale.prototype.script
static bool Locale_script(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsLocale(args.thisv()));

  // Step 3.
  auto* locale = &args.thisv().toObject().as<LocaleObject>();
  JSLinearString* baseName = locale->baseName()->ensureLinear(cx);
  if (!baseName) {
    return false;
  }

  // Step 4 (Unnecessary assertion).

  auto script = BaseNameParts(baseName).script;

  // Step 5.
  if (!script) {
    args.rval().setUndefined();
    return true;
  }

  size_t index = script->index;
  size_t length = script->length;

  // Step 6.
  JSString* str = NewDependentString(cx, baseName, index, length);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

// get Intl.Locale.prototype.script
static bool Locale_script(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsLocale, Locale_script>(cx, args);
}

// get Intl.Locale.prototype.region
static bool Locale_region(JSContext* cx, const CallArgs& args) {
  MOZ_ASSERT(IsLocale(args.thisv()));

  // Step 3.
  auto* locale = &args.thisv().toObject().as<LocaleObject>();
  JSLinearString* baseName = locale->baseName()->ensureLinear(cx);
  if (!baseName) {
    return false;
  }

  // Step 4 (Unnecessary assertion).

  auto region = BaseNameParts(baseName).region;

  // Step 5.
  if (!region) {
    args.rval().setUndefined();
    return true;
  }

  size_t index = region->index;
  size_t length = region->length;

  // Step 6.
  JSString* str = NewDependentString(cx, baseName, index, length);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

// get Intl.Locale.prototype.region
static bool Locale_region(JSContext* cx, unsigned argc, Value* vp) {
  // Steps 1-2.
  CallArgs args = CallArgsFromVp(argc, vp);
  return CallNonGenericMethod<IsLocale, Locale_region>(cx, args);
}

static bool Locale_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().Locale);
  return true;
}

static const JSFunctionSpec locale_methods[] = {
    JS_FN("maximize", Locale_maximize, 0, 0),
    JS_FN("minimize", Locale_minimize, 0, 0),
    JS_FN("toString", Locale_toString, 0, 0),
    JS_FN("toSource", Locale_toSource, 0, 0), JS_FS_END};

static const JSPropertySpec locale_properties[] = {
    JS_PSG("baseName", Locale_baseName, 0),
    JS_PSG("calendar", Locale_calendar, 0),
    JS_PSG("caseFirst", Locale_caseFirst, 0),
    JS_PSG("collation", Locale_collation, 0),
    JS_PSG("hourCycle", Locale_hourCycle, 0),
    JS_PSG("numeric", Locale_numeric, 0),
    JS_PSG("numberingSystem", Locale_numberingSystem, 0),
    JS_PSG("language", Locale_language, 0),
    JS_PSG("script", Locale_script, 0),
    JS_PSG("region", Locale_region, 0),
    JS_STRING_SYM_PS(toStringTag, "Intl.Locale", JSPROP_READONLY),
    JS_PS_END};

const ClassSpec LocaleObject::classSpec_ = {
    GenericCreateConstructor<Locale, 1, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<LocaleObject>,
    nullptr,
    nullptr,
    locale_methods,
    locale_properties,
    nullptr,
    ClassSpec::DontDefineConstructor};

bool js::intl_ValidateAndCanonicalizeLanguageTag(JSContext* cx, unsigned argc,
                                                 Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 2);

  HandleValue tagValue = args[0];
  bool applyToString = args[1].toBoolean();

  if (tagValue.isObject()) {
    JSString* tagStr;
    JS_TRY_VAR_OR_RETURN_FALSE(
        cx, tagStr,
        LanguageTagFromMaybeWrappedLocale(cx, &tagValue.toObject()));
    if (tagStr) {
      args.rval().setString(tagStr);
      return true;
    }
  }

  if (!applyToString && !tagValue.isString()) {
    args.rval().setNull();
    return true;
  }

  JSString* tagStr = ToString(cx, tagValue);
  if (!tagStr) {
    return false;
  }

  Rooted<JSLinearString*> tagLinearStr(cx, tagStr->ensureLinear(cx));
  if (!tagLinearStr) {
    return false;
  }

  // Handle the common case (a standalone language) first.
  // Only the following Unicode BCP 47 locale identifier subset is accepted:
  //   unicode_locale_id = unicode_language_id
  //   unicode_language_id = unicode_language_subtag
  //   unicode_language_subtag = alpha{2,3}
  JSString* language;
  JS_TRY_VAR_OR_RETURN_FALSE(
      cx, language, intl::ParseStandaloneISO639LanguageTag(cx, tagLinearStr));
  if (language) {
    args.rval().setString(language);
    return true;
  }

  mozilla::intl::Locale tag;
  if (!intl::ParseLocale(cx, tagLinearStr, tag)) {
    return false;
  }

  auto result = tag.Canonicalize();
  if (result.isErr()) {
    if (result.unwrapErr() ==
        mozilla::intl::Locale::CanonicalizationError::DuplicateVariant) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DUPLICATE_VARIANT_SUBTAG);
    } else {
      intl::ReportInternalError(cx);
    }
    return false;
  }

  intl::FormatBuffer<char, intl::INITIAL_CHAR_BUFFER_SIZE> buffer(cx);
  if (auto result = tag.ToString(buffer); result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return false;
  }

  JSString* resultStr = buffer.toAsciiString(cx);
  if (!resultStr) {
    return false;
  }

  args.rval().setString(resultStr);
  return true;
}

bool js::intl_TryValidateAndCanonicalizeLanguageTag(JSContext* cx,
                                                    unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  Rooted<JSLinearString*> linear(cx, args[0].toString()->ensureLinear(cx));
  if (!linear) {
    return false;
  }

  mozilla::intl::Locale tag;
  {
    if (!StringIsAscii(linear)) {
      // The caller handles invalid inputs.
      args.rval().setNull();
      return true;
    }

    intl::StringAsciiChars chars(linear);
    if (!chars.init(cx)) {
      return false;
    }

    if (mozilla::intl::LocaleParser::TryParse(chars, tag).isErr()) {
      // The caller handles invalid inputs.
      args.rval().setNull();
      return true;
    }
  }

  auto result = tag.Canonicalize();
  if (result.isErr()) {
    if (result.unwrapErr() ==
        mozilla::intl::Locale::CanonicalizationError::DuplicateVariant) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_DUPLICATE_VARIANT_SUBTAG);
    } else {
      intl::ReportInternalError(cx);
    }
    return false;
  }

  intl::FormatBuffer<char, intl::INITIAL_CHAR_BUFFER_SIZE> buffer(cx);
  if (auto result = tag.ToString(buffer); result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return false;
  }

  JSString* resultStr = buffer.toAsciiString(cx);
  if (!resultStr) {
    return false;
  }
  args.rval().setString(resultStr);
  return true;
}

bool js::intl_ValidateAndCanonicalizeUnicodeExtensionType(JSContext* cx,
                                                          unsigned argc,
                                                          Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);

  HandleValue typeArg = args[0];
  MOZ_ASSERT(typeArg.isString(), "type must be a string");

  HandleValue optionArg = args[1];
  MOZ_ASSERT(optionArg.isString(), "option name must be a string");

  HandleValue keyArg = args[2];
  MOZ_ASSERT(keyArg.isString(), "key must be a string");

  Rooted<JSLinearString*> unicodeType(cx, typeArg.toString()->ensureLinear(cx));
  if (!unicodeType) {
    return false;
  }

  bool isValid;
  if (!IsValidUnicodeExtensionValue(cx, unicodeType, &isValid)) {
    return false;
  }
  if (!isValid) {
    UniqueChars optionChars = EncodeAscii(cx, optionArg.toString());
    if (!optionChars) {
      return false;
    }

    UniqueChars unicodeTypeChars = QuoteString(cx, unicodeType, '"');
    if (!unicodeTypeChars) {
      return false;
    }

    JS_ReportErrorNumberASCII(cx, js::GetErrorMessage, nullptr,
                              JSMSG_INVALID_OPTION_VALUE, optionChars.get(),
                              unicodeTypeChars.get());
    return false;
  }

  char unicodeKey[UnicodeKeyLength];
  {
    JSLinearString* str = keyArg.toString()->ensureLinear(cx);
    if (!str) {
      return false;
    }
    MOZ_ASSERT(str->length() == UnicodeKeyLength);

    for (size_t i = 0; i < UnicodeKeyLength; i++) {
      char16_t ch = str->latin1OrTwoByteChar(i);
      MOZ_ASSERT(mozilla::IsAscii(ch));
      unicodeKey[i] = char(ch);
    }
  }

  UniqueChars unicodeTypeChars = EncodeAscii(cx, unicodeType);
  if (!unicodeTypeChars) {
    return false;
  }

  size_t unicodeTypeLength = unicodeType->length();
  MOZ_ASSERT(strlen(unicodeTypeChars.get()) == unicodeTypeLength);

  // Convert into canonical case before searching for replacements.
  mozilla::intl::AsciiToLowerCase(unicodeTypeChars.get(), unicodeTypeLength,
                                  unicodeTypeChars.get());

  auto key = mozilla::Span(unicodeKey, UnicodeKeyLength);
  auto type = mozilla::Span(unicodeTypeChars.get(), unicodeTypeLength);

  // Search if there's a replacement for the current Unicode keyword.
  JSString* result;
  if (const char* replacement =
          mozilla::intl::Locale::ReplaceUnicodeExtensionType(key, type)) {
    result = NewStringCopyZ<CanGC>(cx, replacement);
  } else {
    result = StringToLowerCase(cx, unicodeType);
  }
  if (!result) {
    return false;
  }

  args.rval().setString(result);
  return true;
}
