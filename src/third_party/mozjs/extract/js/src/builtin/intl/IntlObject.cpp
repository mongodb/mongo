/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implementation of the Intl object and its non-constructor properties. */

#include "builtin/intl/IntlObject.h"

#include "mozilla/Assertions.h"
#include "mozilla/intl/Calendar.h"
#include "mozilla/intl/Collator.h"
#include "mozilla/intl/Currency.h"
#include "mozilla/intl/Locale.h"
#include "mozilla/intl/MeasureUnitGenerated.h"
#include "mozilla/intl/TimeZone.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <string_view>

#include "builtin/Array.h"
#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/FormatBuffer.h"
#include "builtin/intl/NumberingSystemsGenerated.h"
#include "builtin/intl/SharedIntlData.h"
#include "builtin/intl/StringAsciiChars.h"
#include "ds/Sort.h"
#include "js/Class.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/GCAPI.h"
#include "js/GCVector.h"
#include "js/PropertySpec.h"
#include "js/Result.h"
#include "js/StableStringChars.h"
#include "vm/GlobalObject.h"
#include "vm/JSAtomUtils.h"  // ClassName
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/StringType.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

/******************** Intl ********************/

bool js::intl_GetCalendarInfo(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  UniqueChars locale = intl::EncodeLocale(cx, args[0].toString());
  if (!locale) {
    return false;
  }

  auto result = mozilla::intl::Calendar::TryCreate(locale.get());
  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return false;
  }
  auto calendar = result.unwrap();

  RootedObject info(cx, NewPlainObject(cx));
  if (!info) {
    return false;
  }

  RootedValue v(cx);

  v.setInt32(static_cast<int32_t>(calendar->GetFirstDayOfWeek()));
  if (!DefineDataProperty(cx, info, cx->names().firstDayOfWeek, v)) {
    return false;
  }

  v.setInt32(calendar->GetMinimalDaysInFirstWeek());
  if (!DefineDataProperty(cx, info, cx->names().minDays, v)) {
    return false;
  }

  Rooted<ArrayObject*> weekendArray(cx, NewDenseEmptyArray(cx));
  if (!weekendArray) {
    return false;
  }

  auto weekend = calendar->GetWeekend();
  if (weekend.isErr()) {
    intl::ReportInternalError(cx, weekend.unwrapErr());
    return false;
  }

  for (auto day : weekend.unwrap()) {
    if (!NewbornArrayPush(cx, weekendArray,
                          Int32Value(static_cast<int32_t>(day)))) {
      return false;
    }
  }

  v.setObject(*weekendArray);
  if (!DefineDataProperty(cx, info, cx->names().weekend, v)) {
    return false;
  }

  args.rval().setObject(*info);
  return true;
}

static void ReportBadKey(JSContext* cx, JSString* key) {
  if (UniqueChars chars = QuoteString(cx, key, '"')) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INVALID_KEY,
                              chars.get());
  }
}

static bool SameOrParentLocale(JSLinearString* locale,
                               JSLinearString* otherLocale) {
  // Return true if |locale| is the same locale as |otherLocale|.
  if (locale->length() == otherLocale->length()) {
    return EqualStrings(locale, otherLocale);
  }

  // Also return true if |locale| is the parent locale of |otherLocale|.
  if (locale->length() < otherLocale->length()) {
    return HasSubstringAt(otherLocale, locale, 0) &&
           otherLocale->latin1OrTwoByteChar(locale->length()) == '-';
  }

  return false;
}

using SupportedLocaleKind = js::intl::SharedIntlData::SupportedLocaleKind;

// 9.2.2 BestAvailableLocale ( availableLocales, locale )
static JS::Result<JSLinearString*> BestAvailableLocale(
    JSContext* cx, SupportedLocaleKind kind, Handle<JSLinearString*> locale,
    Handle<JSLinearString*> defaultLocale) {
  // In the spec, [[availableLocales]] is formally a list of all available
  // locales. But in our implementation, it's an *incomplete* list, not
  // necessarily including the default locale (and all locales implied by it,
  // e.g. "de" implied by "de-CH"), if that locale isn't in every
  // [[availableLocales]] list (because that locale is supported through
  // fallback, e.g. "de-CH" supported through "de").
  //
  // If we're considering the default locale, augment the spec loop with
  // additional checks to also test whether the current prefix is a prefix of
  // the default locale.

  intl::SharedIntlData& sharedIntlData = cx->runtime()->sharedIntlData.ref();

  auto findLast = [](const auto* chars, size_t length) {
    auto rbegin = std::make_reverse_iterator(chars + length);
    auto rend = std::make_reverse_iterator(chars);
    auto p = std::find(rbegin, rend, '-');

    // |dist(chars, p.base())| is equal to |dist(p, rend)|, pick whichever you
    // find easier to reason about when using reserve iterators.
    ptrdiff_t r = std::distance(chars, p.base());
    MOZ_ASSERT(r == std::distance(p, rend));

    // But always subtract one to convert from the reverse iterator result to
    // the correspoding forward iterator value, because reserve iterators point
    // to one element past the forward iterator value.
    return r - 1;
  };

  // Step 1.
  Rooted<JSLinearString*> candidate(cx, locale);

  // Step 2.
  while (true) {
    // Step 2.a.
    bool supported = false;
    if (!sharedIntlData.isSupportedLocale(cx, kind, candidate, &supported)) {
      return cx->alreadyReportedError();
    }
    if (supported) {
      return candidate.get();
    }

    if (defaultLocale && SameOrParentLocale(candidate, defaultLocale)) {
      return candidate.get();
    }

    // Step 2.b.
    ptrdiff_t pos;
    if (candidate->hasLatin1Chars()) {
      JS::AutoCheckCannotGC nogc;
      pos = findLast(candidate->latin1Chars(nogc), candidate->length());
    } else {
      JS::AutoCheckCannotGC nogc;
      pos = findLast(candidate->twoByteChars(nogc), candidate->length());
    }

    if (pos < 0) {
      return nullptr;
    }

    // Step 2.c.
    size_t length = size_t(pos);
    if (length >= 2 && candidate->latin1OrTwoByteChar(length - 2) == '-') {
      length -= 2;
    }

    // Step 2.d.
    candidate = NewDependentString(cx, candidate, 0, length);
    if (!candidate) {
      return cx->alreadyReportedError();
    }
  }
}

// 9.2.2 BestAvailableLocale ( availableLocales, locale )
//
// Carries an additional third argument in our implementation to provide the
// default locale. See the doc-comment in the header file.
bool js::intl_BestAvailableLocale(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);

  SupportedLocaleKind kind;
  {
    JSLinearString* typeStr = args[0].toString()->ensureLinear(cx);
    if (!typeStr) {
      return false;
    }

    if (StringEqualsLiteral(typeStr, "Collator")) {
      kind = SupportedLocaleKind::Collator;
    } else if (StringEqualsLiteral(typeStr, "DateTimeFormat")) {
      kind = SupportedLocaleKind::DateTimeFormat;
    } else if (StringEqualsLiteral(typeStr, "DisplayNames")) {
      kind = SupportedLocaleKind::DisplayNames;
    } else if (StringEqualsLiteral(typeStr, "ListFormat")) {
      kind = SupportedLocaleKind::ListFormat;
    } else if (StringEqualsLiteral(typeStr, "NumberFormat")) {
      kind = SupportedLocaleKind::NumberFormat;
    } else if (StringEqualsLiteral(typeStr, "PluralRules")) {
      kind = SupportedLocaleKind::PluralRules;
    } else if (StringEqualsLiteral(typeStr, "RelativeTimeFormat")) {
      kind = SupportedLocaleKind::RelativeTimeFormat;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(typeStr, "Segmenter"));
      kind = SupportedLocaleKind::Segmenter;
    }
  }

  Rooted<JSLinearString*> locale(cx, args[1].toString()->ensureLinear(cx));
  if (!locale) {
    return false;
  }

#ifdef DEBUG
  {
    MOZ_ASSERT(StringIsAscii(locale), "language tags are ASCII-only");

    // |locale| is a structurally valid language tag.
    mozilla::intl::Locale tag;

    using ParserError = mozilla::intl::LocaleParser::ParserError;
    mozilla::Result<mozilla::Ok, ParserError> parse_result = Ok();
    {
      intl::StringAsciiChars chars(locale);
      if (!chars.init(cx)) {
        return false;
      }

      parse_result = mozilla::intl::LocaleParser::TryParse(chars, tag);
    }

    if (parse_result.isErr()) {
      MOZ_ASSERT(parse_result.unwrapErr() == ParserError::OutOfMemory,
                 "locale is a structurally valid language tag");

      intl::ReportInternalError(cx);
      return false;
    }

    MOZ_ASSERT(!tag.GetUnicodeExtension(),
               "locale must contain no Unicode extensions");

    if (auto result = tag.Canonicalize(); result.isErr()) {
      MOZ_ASSERT(
          result.unwrapErr() !=
          mozilla::intl::Locale::CanonicalizationError::DuplicateVariant);
      intl::ReportInternalError(cx);
      return false;
    }

    intl::FormatBuffer<char, intl::INITIAL_CHAR_BUFFER_SIZE> buffer(cx);
    if (auto result = tag.ToString(buffer); result.isErr()) {
      intl::ReportInternalError(cx, result.unwrapErr());
      return false;
    }

    JSLinearString* tagStr = buffer.toString(cx);
    if (!tagStr) {
      return false;
    }

    MOZ_ASSERT(EqualStrings(locale, tagStr),
               "locale is a canonicalized language tag");
  }
#endif

  MOZ_ASSERT(args[2].isNull() || args[2].isString());

  Rooted<JSLinearString*> defaultLocale(cx);
  if (args[2].isString()) {
    defaultLocale = args[2].toString()->ensureLinear(cx);
    if (!defaultLocale) {
      return false;
    }
  }

  JSString* result;
  JS_TRY_VAR_OR_RETURN_FALSE(
      cx, result, BestAvailableLocale(cx, kind, locale, defaultLocale));

  if (result) {
    args.rval().setString(result);
  } else {
    args.rval().setUndefined();
  }
  return true;
}

bool js::intl_supportedLocaleOrFallback(JSContext* cx, unsigned argc,
                                        Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  Rooted<JSLinearString*> locale(cx, args[0].toString()->ensureLinear(cx));
  if (!locale) {
    return false;
  }

  mozilla::intl::Locale tag;
  bool canParseLocale = false;
  if (StringIsAscii(locale)) {
    intl::StringAsciiChars chars(locale);
    if (!chars.init(cx)) {
      return false;
    }

    // Tell the analysis the |tag.canonicalize()| method can't GC.
    JS::AutoSuppressGCAnalysis nogc;

    canParseLocale = mozilla::intl::LocaleParser::TryParse(chars, tag).isOk() &&
                     tag.Canonicalize().isOk();
  }

  Rooted<JSLinearString*> candidate(cx);
  if (!canParseLocale) {
    candidate = NewStringCopyZ<CanGC>(cx, intl::LastDitchLocale());
    if (!candidate) {
      return false;
    }
  } else {
    // The default locale must be in [[AvailableLocales]], and that list must
    // not contain any locales with Unicode extension sequences, so remove any
    // present in the candidate.
    tag.ClearUnicodeExtension();

    intl::FormatBuffer<char, intl::INITIAL_CHAR_BUFFER_SIZE> buffer(cx);
    if (auto result = tag.ToString(buffer); result.isErr()) {
      intl::ReportInternalError(cx, result.unwrapErr());
      return false;
    }

    candidate = buffer.toAsciiString(cx);
    if (!candidate) {
      return false;
    }

    // Certain old-style language tags lack a script code, but in current
    // usage they *would* include a script code. Map these over to modern
    // forms.
    for (const auto& mapping : js::intl::oldStyleLanguageTagMappings) {
      const char* oldStyle = mapping.oldStyle;
      const char* modernStyle = mapping.modernStyle;

      if (StringEqualsAscii(candidate, oldStyle)) {
        candidate = NewStringCopyZ<CanGC>(cx, modernStyle);
        if (!candidate) {
          return false;
        }
        break;
      }
    }
  }

  // 9.1 Internal slots of Service Constructors
  //
  // - [[AvailableLocales]] is a List [...]. The list must include the value
  //   returned by the DefaultLocale abstract operation (6.2.4), [...].
  //
  // That implies we must ignore any candidate which isn't supported by all
  // Intl service constructors.

  Rooted<JSLinearString*> supportedCollator(cx);
  JS_TRY_VAR_OR_RETURN_FALSE(
      cx, supportedCollator,
      BestAvailableLocale(cx, SupportedLocaleKind::Collator, candidate,
                          nullptr));

  Rooted<JSLinearString*> supportedDateTimeFormat(cx);
  JS_TRY_VAR_OR_RETURN_FALSE(
      cx, supportedDateTimeFormat,
      BestAvailableLocale(cx, SupportedLocaleKind::DateTimeFormat, candidate,
                          nullptr));

#ifdef DEBUG
  // Note: We don't test the supported locales of the remaining Intl service
  // constructors, because the set of supported locales is exactly equal to
  // the set of supported locales of Intl.DateTimeFormat.
  for (auto kind : {
           SupportedLocaleKind::DisplayNames,
           SupportedLocaleKind::ListFormat,
           SupportedLocaleKind::NumberFormat,
           SupportedLocaleKind::PluralRules,
           SupportedLocaleKind::RelativeTimeFormat,
           SupportedLocaleKind::Segmenter,
       }) {
    JSLinearString* supported;
    JS_TRY_VAR_OR_RETURN_FALSE(
        cx, supported, BestAvailableLocale(cx, kind, candidate, nullptr));

    MOZ_ASSERT(!!supported == !!supportedDateTimeFormat);
    MOZ_ASSERT_IF(supported, EqualStrings(supported, supportedDateTimeFormat));
  }
#endif

  // Accept the candidate locale if it is supported by all Intl service
  // constructors.
  if (supportedCollator && supportedDateTimeFormat) {
    // Use the actually supported locale instead of the candidate locale. For
    // example when the candidate locale "en-US-posix" is supported through
    // "en-US", use "en-US" as the default locale.
    //
    // Also prefer the supported locale with more subtags. For example when
    // requesting "de-CH" and Intl.DateTimeFormat supports "de-CH", but
    // Intl.Collator only "de", still return "de-CH" as the result.
    if (SameOrParentLocale(supportedCollator, supportedDateTimeFormat)) {
      candidate = supportedDateTimeFormat;
    } else {
      candidate = supportedCollator;
    }
  } else {
    candidate = NewStringCopyZ<CanGC>(cx, intl::LastDitchLocale());
    if (!candidate) {
      return false;
    }
  }

  args.rval().setString(candidate);
  return true;
}

using StringList = GCVector<JSLinearString*>;

/**
 * Create a sorted array from a list of strings.
 */
static ArrayObject* CreateArrayFromList(JSContext* cx,
                                        MutableHandle<StringList> list) {
  // Reserve scratch space for MergeSort().
  size_t initialLength = list.length();
  if (!list.growBy(initialLength)) {
    return nullptr;
  }

  // Sort all strings in alphabetical order.
  MOZ_ALWAYS_TRUE(
      MergeSort(list.begin(), initialLength, list.begin() + initialLength,
                [](const auto* a, const auto* b, bool* lessOrEqual) {
                  *lessOrEqual = CompareStrings(a, b) <= 0;
                  return true;
                }));

  // Ensure we don't add duplicate entries to the array.
  auto* end = std::unique(
      list.begin(), list.begin() + initialLength,
      [](const auto* a, const auto* b) { return EqualStrings(a, b); });

  // std::unique leaves the elements after |end| with an unspecified value, so
  // remove them first. And also delete the elements in the scratch space.
  list.shrinkBy(std::distance(end, list.end()));

  // And finally copy the strings into the result array.
  auto* array = NewDenseFullyAllocatedArray(cx, list.length());
  if (!array) {
    return nullptr;
  }
  array->setDenseInitializedLength(list.length());

  for (size_t i = 0; i < list.length(); ++i) {
    array->initDenseElement(i, StringValue(list[i]));
  }

  return array;
}

/**
 * Create an array from a sorted list of strings.
 */
template <size_t N>
static ArrayObject* CreateArrayFromSortedList(
    JSContext* cx, const std::array<const char*, N>& list) {
  // Ensure the list is sorted and doesn't contain duplicates.
#ifdef DEBUG
  // See bug 1583449 for why the lambda can't be in the MOZ_ASSERT.
  auto isLargerThanOrEqual = [](const auto& a, const auto& b) {
    return std::strcmp(a, b) >= 0;
  };
#endif
  MOZ_ASSERT(std::adjacent_find(std::begin(list), std::end(list),
                                isLargerThanOrEqual) == std::end(list));

  size_t length = std::size(list);

  Rooted<ArrayObject*> array(cx, NewDenseFullyAllocatedArray(cx, length));
  if (!array) {
    return nullptr;
  }
  array->ensureDenseInitializedLength(0, length);

  for (size_t i = 0; i < length; ++i) {
    auto* str = NewStringCopyZ<CanGC>(cx, list[i]);
    if (!str) {
      return nullptr;
    }
    array->initDenseElement(i, StringValue(str));
  }
  return array;
}

/**
 * Create an array from an intl::Enumeration.
 */
template <const auto& unsupported, class Enumeration>
static bool EnumerationIntoList(JSContext* cx, Enumeration values,
                                MutableHandle<StringList> list) {
  for (auto value : values) {
    if (value.isErr()) {
      intl::ReportInternalError(cx);
      return false;
    }
    auto span = value.unwrap();

    // Skip over known, unsupported values.
    std::string_view sv(span.data(), span.size());
    if (std::any_of(std::begin(unsupported), std::end(unsupported),
                    [sv](const auto& e) { return sv == e; })) {
      continue;
    }

    auto* string = NewStringCopy<CanGC>(cx, span);
    if (!string) {
      return false;
    }
    if (!list.append(string)) {
      return false;
    }
  }

  return true;
}

/**
 * Returns the list of calendar types which mustn't be returned by
 * |Intl.supportedValuesOf()|.
 */
static constexpr auto UnsupportedCalendars() {
  // No calendar values are currently unsupported.
  return std::array<const char*, 0>{};
}

// Defined outside of the function to workaround bugs in GCC<9.
// Also see <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=85589>.
static constexpr auto UnsupportedCalendarsArray = UnsupportedCalendars();

/**
 * AvailableCalendars ( )
 */
static ArrayObject* AvailableCalendars(JSContext* cx) {
  Rooted<StringList> list(cx, StringList(cx));

  {
    // Hazard analysis complains that the mozilla::Result destructor calls a
    // GC function, which is unsound when returning an unrooted value. Work
    // around this issue by restricting the lifetime of |keywords| to a
    // separate block.
    auto keywords = mozilla::intl::Calendar::GetBcp47KeywordValuesForLocale("");
    if (keywords.isErr()) {
      intl::ReportInternalError(cx, keywords.unwrapErr());
      return nullptr;
    }

    static constexpr auto& unsupported = UnsupportedCalendarsArray;

    if (!EnumerationIntoList<unsupported>(cx, keywords.unwrap(), &list)) {
      return nullptr;
    }
  }

  return CreateArrayFromList(cx, &list);
}

/**
 * Returns the list of collation types which mustn't be returned by
 * |Intl.supportedValuesOf()|.
 */
static constexpr auto UnsupportedCollations() {
  return std::array{
      "search",
      "standard",
  };
}

// Defined outside of the function to workaround bugs in GCC<9.
// Also see <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=85589>.
static constexpr auto UnsupportedCollationsArray = UnsupportedCollations();

/**
 * AvailableCollations ( )
 */
static ArrayObject* AvailableCollations(JSContext* cx) {
  Rooted<StringList> list(cx, StringList(cx));

  {
    // Hazard analysis complains that the mozilla::Result destructor calls a
    // GC function, which is unsound when returning an unrooted value. Work
    // around this issue by restricting the lifetime of |keywords| to a
    // separate block.
    auto keywords = mozilla::intl::Collator::GetBcp47KeywordValues();
    if (keywords.isErr()) {
      intl::ReportInternalError(cx, keywords.unwrapErr());
      return nullptr;
    }

    static constexpr auto& unsupported = UnsupportedCollationsArray;

    if (!EnumerationIntoList<unsupported>(cx, keywords.unwrap(), &list)) {
      return nullptr;
    }
  }

  return CreateArrayFromList(cx, &list);
}

/**
 * Returns a list of known, unsupported currencies which are returned by
 * |Currency::GetISOCurrencies()|.
 */
static constexpr auto UnsupportedCurrencies() {
  // "MVP" is also marked with "questionable, remove?" in ucurr.cpp, but only
  // this single currency code isn't supported by |Intl.DisplayNames| and
  // therefore must be excluded by |Intl.supportedValuesOf|.
  return std::array{
      "LSM",  // https://unicode-org.atlassian.net/browse/ICU-21687
  };
}

/**
 * Return a list of known, missing currencies which aren't returned by
 * |Currency::GetISOCurrencies()|.
 */
static constexpr auto MissingCurrencies() {
  return std::array{
      "SLE",  // https://unicode-org.atlassian.net/browse/ICU-21989
      "VED",  // https://unicode-org.atlassian.net/browse/ICU-21989
  };
}

// Defined outside of the function to workaround bugs in GCC<9.
// Also see <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=85589>.
static constexpr auto UnsupportedCurrenciesArray = UnsupportedCurrencies();
static constexpr auto MissingCurrenciesArray = MissingCurrencies();

/**
 * AvailableCurrencies ( )
 */
static ArrayObject* AvailableCurrencies(JSContext* cx) {
  Rooted<StringList> list(cx, StringList(cx));

  {
    // Hazard analysis complains that the mozilla::Result destructor calls a
    // GC function, which is unsound when returning an unrooted value. Work
    // around this issue by restricting the lifetime of |currencies| to a
    // separate block.
    auto currencies = mozilla::intl::Currency::GetISOCurrencies();
    if (currencies.isErr()) {
      intl::ReportInternalError(cx, currencies.unwrapErr());
      return nullptr;
    }

    static constexpr auto& unsupported = UnsupportedCurrenciesArray;

    if (!EnumerationIntoList<unsupported>(cx, currencies.unwrap(), &list)) {
      return nullptr;
    }
  }

  // Add known missing values.
  for (const char* value : MissingCurrenciesArray) {
    auto* string = NewStringCopyZ<CanGC>(cx, value);
    if (!string) {
      return nullptr;
    }
    if (!list.append(string)) {
      return nullptr;
    }
  }

  return CreateArrayFromList(cx, &list);
}

/**
 * AvailableNumberingSystems ( )
 */
static ArrayObject* AvailableNumberingSystems(JSContext* cx) {
  static constexpr std::array numberingSystems = {
      NUMBERING_SYSTEMS_WITH_SIMPLE_DIGIT_MAPPINGS};

  return CreateArrayFromSortedList(cx, numberingSystems);
}

/**
 * AvailableTimeZones ( )
 */
static ArrayObject* AvailableTimeZones(JSContext* cx) {
  // Unsorted list of canonical time zone names, possibly containing
  // duplicates.
  Rooted<StringList> timeZones(cx, StringList(cx));

  intl::SharedIntlData& sharedIntlData = cx->runtime()->sharedIntlData.ref();
  auto iterResult = sharedIntlData.availableTimeZonesIteration(cx);
  if (iterResult.isErr()) {
    return nullptr;
  }
  auto iter = iterResult.unwrap();

  Rooted<JSAtom*> validatedTimeZone(cx);
  Rooted<JSAtom*> ianaTimeZone(cx);
  for (; !iter.done(); iter.next()) {
    validatedTimeZone = iter.get();

    // Canonicalize the time zone before adding it to the result array.

    // Some time zone names are canonicalized differently by ICU -- handle
    // those first.
    ianaTimeZone.set(nullptr);
    if (!sharedIntlData.tryCanonicalizeTimeZoneConsistentWithIANA(
            cx, validatedTimeZone, &ianaTimeZone)) {
      return nullptr;
    }

    JSLinearString* timeZone;
    if (ianaTimeZone) {
      cx->markAtom(ianaTimeZone);

      timeZone = ianaTimeZone;
    } else {
      // Call into ICU to canonicalize the time zone.

      JS::AutoStableStringChars stableChars(cx);
      if (!stableChars.initTwoByte(cx, validatedTimeZone)) {
        return nullptr;
      }

      intl::FormatBuffer<char16_t, intl::INITIAL_CHAR_BUFFER_SIZE>
          canonicalTimeZone(cx);
      auto result = mozilla::intl::TimeZone::GetCanonicalTimeZoneID(
          stableChars.twoByteRange(), canonicalTimeZone);
      if (result.isErr()) {
        intl::ReportInternalError(cx, result.unwrapErr());
        return nullptr;
      }

      timeZone = canonicalTimeZone.toString(cx);
      if (!timeZone) {
        return nullptr;
      }

      // Canonicalize both to "UTC" per CanonicalizeTimeZoneName().
      if (StringEqualsLiteral(timeZone, "Etc/UTC") ||
          StringEqualsLiteral(timeZone, "Etc/GMT")) {
        timeZone = cx->names().UTC;
      }
    }

    if (!timeZones.append(timeZone)) {
      return nullptr;
    }
  }

  return CreateArrayFromList(cx, &timeZones);
}

template <size_t N>
constexpr auto MeasurementUnitNames(
    const mozilla::intl::SimpleMeasureUnit (&units)[N]) {
  std::array<const char*, N> array = {};
  for (size_t i = 0; i < N; ++i) {
    array[i] = units[i].name;
  }
  return array;
}

/**
 * AvailableUnits ( )
 */
static ArrayObject* AvailableUnits(JSContext* cx) {
  static constexpr auto simpleMeasureUnitNames =
      MeasurementUnitNames(mozilla::intl::simpleMeasureUnits);

  return CreateArrayFromSortedList(cx, simpleMeasureUnitNames);
}

bool js::intl_SupportedValuesOf(JSContext* cx, unsigned argc, JS::Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isString());

  JSLinearString* key = args[0].toString()->ensureLinear(cx);
  if (!key) {
    return false;
  }

  ArrayObject* list;
  if (StringEqualsLiteral(key, "calendar")) {
    list = AvailableCalendars(cx);
  } else if (StringEqualsLiteral(key, "collation")) {
    list = AvailableCollations(cx);
  } else if (StringEqualsLiteral(key, "currency")) {
    list = AvailableCurrencies(cx);
  } else if (StringEqualsLiteral(key, "numberingSystem")) {
    list = AvailableNumberingSystems(cx);
  } else if (StringEqualsLiteral(key, "timeZone")) {
    list = AvailableTimeZones(cx);
  } else if (StringEqualsLiteral(key, "unit")) {
    list = AvailableUnits(cx);
  } else {
    ReportBadKey(cx, key);
    return false;
  }
  if (!list) {
    return false;
  }

  args.rval().setObject(*list);
  return true;
}

static bool intl_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().Intl);
  return true;
}

static const JSFunctionSpec intl_static_methods[] = {
    JS_FN("toSource", intl_toSource, 0, 0),
    JS_SELF_HOSTED_FN("getCanonicalLocales", "Intl_getCanonicalLocales", 1, 0),
    JS_SELF_HOSTED_FN("supportedValuesOf", "Intl_supportedValuesOf", 1, 0),
    JS_FS_END};

static const JSPropertySpec intl_static_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "Intl", JSPROP_READONLY), JS_PS_END};

static JSObject* CreateIntlObject(JSContext* cx, JSProtoKey key) {
  RootedObject proto(cx, &cx->global()->getObjectPrototype());

  // The |Intl| object is just a plain object with some "static" function
  // properties and some constructor properties.
  return NewTenuredObjectWithGivenProto(cx, &IntlClass, proto);
}

/**
 * Initializes the Intl Object and its standard built-in properties.
 * Spec: ECMAScript Internationalization API Specification, 8.0, 8.1
 */
static bool IntlClassFinish(JSContext* cx, HandleObject intl,
                            HandleObject proto) {
  // Add the constructor properties.
  RootedId ctorId(cx);
  RootedValue ctorValue(cx);
  for (const auto& protoKey : {
           JSProto_Collator,
           JSProto_DateTimeFormat,
           JSProto_DisplayNames,
           JSProto_ListFormat,
           JSProto_Locale,
           JSProto_NumberFormat,
           JSProto_PluralRules,
           JSProto_RelativeTimeFormat,
           JSProto_Segmenter,
       }) {
    if (GlobalObject::skipDeselectedConstructor(cx, protoKey)) {
      continue;
    }

    JSObject* ctor = GlobalObject::getOrCreateConstructor(cx, protoKey);
    if (!ctor) {
      return false;
    }

    ctorId = NameToId(ClassName(protoKey, cx));
    ctorValue.setObject(*ctor);
    if (!DefineDataProperty(cx, intl, ctorId, ctorValue, 0)) {
      return false;
    }
  }

  return true;
}

static const ClassSpec IntlClassSpec = {
    CreateIntlObject, nullptr, intl_static_methods, intl_static_properties,
    nullptr,          nullptr, IntlClassFinish};

const JSClass js::IntlClass = {"Intl", JSCLASS_HAS_CACHED_PROTO(JSProto_Intl),
                               JS_NULL_CLASS_OPS, &IntlClassSpec};
