/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implementation of the Intl object and its non-constructor properties. */

#include "builtin/intl/IntlObject.h"

#include "mozilla/Assertions.h"
#include "mozilla/Likely.h"
#include "mozilla/Range.h"

#include <algorithm>
#include <iterator>

#include "jsapi.h"

#include "builtin/Array.h"
#include "builtin/intl/Collator.h"
#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/DateTimeFormat.h"
#include "builtin/intl/LanguageTag.h"
#include "builtin/intl/NumberFormat.h"
#include "builtin/intl/PluralRules.h"
#include "builtin/intl/RelativeTimeFormat.h"
#include "builtin/intl/ScopedICUObject.h"
#include "builtin/intl/SharedIntlData.h"
#include "js/CharacterEncoding.h"
#include "js/Class.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/PropertySpec.h"
#include "js/Result.h"
#include "js/StableStringChars.h"
#include "unicode/ucal.h"
#include "unicode/udat.h"
#include "unicode/udatpg.h"
#include "unicode/uloc.h"
#include "unicode/utypes.h"
#include "vm/GlobalObject.h"
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/StringType.h"
#include "vm/WellKnownAtom.h"  // js_*_str

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using mozilla::Range;
using mozilla::RangedPtr;

using JS::AutoStableStringChars;

using js::intl::CallICU;
using js::intl::DateTimeFormatOptions;
using js::intl::IcuLocale;

/******************** Intl ********************/

bool js::intl_GetCalendarInfo(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  UniqueChars locale = intl::EncodeLocale(cx, args[0].toString());
  if (!locale) {
    return false;
  }

  UErrorCode status = U_ZERO_ERROR;
  const UChar* uTimeZone = nullptr;
  int32_t uTimeZoneLength = 0;
  UCalendar* cal = ucal_open(uTimeZone, uTimeZoneLength, locale.get(),
                             UCAL_DEFAULT, &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }
  ScopedICUObject<UCalendar, ucal_close> toClose(cal);

  RootedObject info(cx, NewBuiltinClassInstance<PlainObject>(cx));
  if (!info) {
    return false;
  }

  RootedValue v(cx);
  int32_t firstDayOfWeek = ucal_getAttribute(cal, UCAL_FIRST_DAY_OF_WEEK);
  v.setInt32(firstDayOfWeek);

  if (!DefineDataProperty(cx, info, cx->names().firstDayOfWeek, v)) {
    return false;
  }

  int32_t minDays = ucal_getAttribute(cal, UCAL_MINIMAL_DAYS_IN_FIRST_WEEK);
  v.setInt32(minDays);
  if (!DefineDataProperty(cx, info, cx->names().minDays, v)) {
    return false;
  }

  UCalendarWeekdayType prevDayType =
      ucal_getDayOfWeekType(cal, UCAL_SATURDAY, &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }

  RootedValue weekendStart(cx), weekendEnd(cx);

  for (int i = UCAL_SUNDAY; i <= UCAL_SATURDAY; i++) {
    UCalendarDaysOfWeek dayOfWeek = static_cast<UCalendarDaysOfWeek>(i);
    UCalendarWeekdayType type = ucal_getDayOfWeekType(cal, dayOfWeek, &status);
    if (U_FAILURE(status)) {
      intl::ReportInternalError(cx);
      return false;
    }

    if (prevDayType != type) {
      switch (type) {
        case UCAL_WEEKDAY:
          // If the first Weekday after Weekend is Sunday (1),
          // then the last Weekend day is Saturday (7).
          // Otherwise we'll just take the previous days number.
          weekendEnd.setInt32(i == 1 ? 7 : i - 1);
          break;
        case UCAL_WEEKEND:
          weekendStart.setInt32(i);
          break;
        case UCAL_WEEKEND_ONSET:
        case UCAL_WEEKEND_CEASE:
          // At the time this code was added, ICU apparently never behaves this
          // way, so just throw, so that users will report a bug and we can
          // decide what to do.
          intl::ReportInternalError(cx);
          return false;
        default:
          break;
      }
    }

    prevDayType = type;
  }

  MOZ_ASSERT(weekendStart.isInt32());
  MOZ_ASSERT(weekendEnd.isInt32());

  if (!DefineDataProperty(cx, info, cx->names().weekendStart, weekendStart)) {
    return false;
  }

  if (!DefineDataProperty(cx, info, cx->names().weekendEnd, weekendEnd)) {
    return false;
  }

  args.rval().setObject(*info);
  return true;
}

static void ReportBadKey(JSContext* cx, HandleString key) {
  if (UniqueChars chars = QuoteString(cx, key, '"')) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_INVALID_KEY,
                              chars.get());
  }
}

template <typename ConstChar>
static bool MatchPart(RangedPtr<ConstChar> iter, const RangedPtr<ConstChar> end,
                      const char* part, size_t partlen) {
  for (size_t i = 0; i < partlen; iter++, i++) {
    if (iter == end || *iter != part[i]) {
      return false;
    }
  }

  return true;
}

template <typename ConstChar, size_t N>
inline bool MatchPart(RangedPtr<ConstChar>* iter,
                      const RangedPtr<ConstChar> end, const char (&part)[N]) {
  if (!MatchPart(*iter, end, part, N - 1)) {
    return false;
  }

  *iter += N - 1;
  return true;
}

enum class DisplayNameStyle {
  Narrow,
  Short,
  Long,
};

template <typename ConstChar>
static JSString* ComputeSingleDisplayName(JSContext* cx, UDateFormat* fmt,
                                          UDateTimePatternGenerator* dtpg,
                                          DisplayNameStyle style,
                                          const Range<ConstChar>& pattern,
                                          HandleString patternString) {
  RangedPtr<ConstChar> iter = pattern.begin();
  const RangedPtr<ConstChar> end = pattern.end();

  auto MatchSlash = [cx, patternString, &iter, end]() {
    if (MOZ_LIKELY(iter != end && *iter == '/')) {
      iter++;
      return true;
    }

    ReportBadKey(cx, patternString);
    return false;
  };

  if (!MatchPart(&iter, end, "dates")) {
    ReportBadKey(cx, patternString);
    return nullptr;
  }

  if (!MatchSlash()) {
    return nullptr;
  }

  if (MatchPart(&iter, end, "fields")) {
    if (!MatchSlash()) {
      return nullptr;
    }

    UDateTimePatternField fieldType;

    if (MatchPart(&iter, end, "year")) {
      fieldType = UDATPG_YEAR_FIELD;
    } else if (MatchPart(&iter, end, "month")) {
      fieldType = UDATPG_MONTH_FIELD;
    } else if (MatchPart(&iter, end, "week")) {
      fieldType = UDATPG_WEEK_OF_YEAR_FIELD;
    } else if (MatchPart(&iter, end, "day")) {
      fieldType = UDATPG_DAY_FIELD;
    } else {
      ReportBadKey(cx, patternString);
      return nullptr;
    }

    // This part must be the final part with no trailing data.
    if (iter != end) {
      ReportBadKey(cx, patternString);
      return nullptr;
    }

    int32_t resultSize;
    const UChar* value = udatpg_getAppendItemName(dtpg, fieldType, &resultSize);
    MOZ_ASSERT(resultSize >= 0);

    return NewStringCopyN<CanGC>(cx, value, size_t(resultSize));
  }

  if (MatchPart(&iter, end, "gregorian")) {
    if (!MatchSlash()) {
      return nullptr;
    }

    UDateFormatSymbolType symbolType;
    int32_t index;

    if (MatchPart(&iter, end, "months")) {
      if (!MatchSlash()) {
        return nullptr;
      }

      switch (style) {
        case DisplayNameStyle::Narrow:
          symbolType = UDAT_STANDALONE_NARROW_MONTHS;
          break;

        case DisplayNameStyle::Short:
          symbolType = UDAT_STANDALONE_SHORT_MONTHS;
          break;

        case DisplayNameStyle::Long:
          symbolType = UDAT_STANDALONE_MONTHS;
          break;
      }

      if (MatchPart(&iter, end, "january")) {
        index = UCAL_JANUARY;
      } else if (MatchPart(&iter, end, "february")) {
        index = UCAL_FEBRUARY;
      } else if (MatchPart(&iter, end, "march")) {
        index = UCAL_MARCH;
      } else if (MatchPart(&iter, end, "april")) {
        index = UCAL_APRIL;
      } else if (MatchPart(&iter, end, "may")) {
        index = UCAL_MAY;
      } else if (MatchPart(&iter, end, "june")) {
        index = UCAL_JUNE;
      } else if (MatchPart(&iter, end, "july")) {
        index = UCAL_JULY;
      } else if (MatchPart(&iter, end, "august")) {
        index = UCAL_AUGUST;
      } else if (MatchPart(&iter, end, "september")) {
        index = UCAL_SEPTEMBER;
      } else if (MatchPart(&iter, end, "october")) {
        index = UCAL_OCTOBER;
      } else if (MatchPart(&iter, end, "november")) {
        index = UCAL_NOVEMBER;
      } else if (MatchPart(&iter, end, "december")) {
        index = UCAL_DECEMBER;
      } else {
        ReportBadKey(cx, patternString);
        return nullptr;
      }
    } else if (MatchPart(&iter, end, "weekdays")) {
      if (!MatchSlash()) {
        return nullptr;
      }

      switch (style) {
        case DisplayNameStyle::Narrow:
          symbolType = UDAT_STANDALONE_NARROW_WEEKDAYS;
          break;

        case DisplayNameStyle::Short:
          symbolType = UDAT_STANDALONE_SHORT_WEEKDAYS;
          break;

        case DisplayNameStyle::Long:
          symbolType = UDAT_STANDALONE_WEEKDAYS;
          break;
      }

      if (MatchPart(&iter, end, "monday")) {
        index = UCAL_MONDAY;
      } else if (MatchPart(&iter, end, "tuesday")) {
        index = UCAL_TUESDAY;
      } else if (MatchPart(&iter, end, "wednesday")) {
        index = UCAL_WEDNESDAY;
      } else if (MatchPart(&iter, end, "thursday")) {
        index = UCAL_THURSDAY;
      } else if (MatchPart(&iter, end, "friday")) {
        index = UCAL_FRIDAY;
      } else if (MatchPart(&iter, end, "saturday")) {
        index = UCAL_SATURDAY;
      } else if (MatchPart(&iter, end, "sunday")) {
        index = UCAL_SUNDAY;
      } else {
        ReportBadKey(cx, patternString);
        return nullptr;
      }
    } else if (MatchPart(&iter, end, "dayperiods")) {
      if (!MatchSlash()) {
        return nullptr;
      }

      symbolType = UDAT_AM_PMS;

      if (MatchPart(&iter, end, "am")) {
        index = UCAL_AM;
      } else if (MatchPart(&iter, end, "pm")) {
        index = UCAL_PM;
      } else {
        ReportBadKey(cx, patternString);
        return nullptr;
      }
    } else {
      ReportBadKey(cx, patternString);
      return nullptr;
    }

    // This part must be the final part with no trailing data.
    if (iter != end) {
      ReportBadKey(cx, patternString);
      return nullptr;
    }

    return CallICU(cx, [fmt, symbolType, index](UChar* chars, int32_t size,
                                                UErrorCode* status) {
      return udat_getSymbols(fmt, symbolType, index, chars, size, status);
    });
  }

  ReportBadKey(cx, patternString);
  return nullptr;
}

bool js::intl_ComputeDisplayNames(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);

  // 1. Assert: locale is a string.
  UniqueChars locale = intl::EncodeLocale(cx, args[0].toString());
  if (!locale) {
    return false;
  }

  // 2. Assert: style is a string.
  DisplayNameStyle dnStyle;
  {
    JSLinearString* style = args[1].toString()->ensureLinear(cx);
    if (!style) {
      return false;
    }

    if (StringEqualsLiteral(style, "narrow")) {
      dnStyle = DisplayNameStyle::Narrow;
    } else if (StringEqualsLiteral(style, "short")) {
      dnStyle = DisplayNameStyle::Short;
    } else {
      MOZ_ASSERT(StringEqualsLiteral(style, "long"));
      dnStyle = DisplayNameStyle::Long;
    }
  }

  // 3. Assert: keys is an Array.
  RootedArrayObject keys(cx, &args[2].toObject().as<ArrayObject>());
  if (!keys) {
    return false;
  }

  // 4. Let result be ArrayCreate(0).
  RootedArrayObject result(cx, NewDenseFullyAllocatedArray(cx, keys->length()));
  if (!result) {
    return false;
  }
  result->ensureDenseInitializedLength(0, keys->length());

  UErrorCode status = U_ZERO_ERROR;

  UDateFormat* fmt =
      udat_open(UDAT_DEFAULT, UDAT_DEFAULT, IcuLocale(locale.get()), nullptr, 0,
                nullptr, 0, &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }
  ScopedICUObject<UDateFormat, udat_close> datToClose(fmt);

  // UDateTimePatternGenerator will be needed for translations of date and
  // time fields like "month", "week", "day" etc.
  UDateTimePatternGenerator* dtpg =
      udatpg_open(IcuLocale(locale.get()), &status);
  if (U_FAILURE(status)) {
    intl::ReportInternalError(cx);
    return false;
  }
  ScopedICUObject<UDateTimePatternGenerator, udatpg_close> datPgToClose(dtpg);

  // 5. For each element of keys,
  RootedString keyValStr(cx);
  RootedValue v(cx);
  for (uint32_t i = 0; i < keys->length(); i++) {
    if (!GetElement(cx, keys, keys, i, &v)) {
      return false;
    }

    keyValStr = v.toString();

    AutoStableStringChars stablePatternChars(cx);
    if (!stablePatternChars.init(cx, keyValStr)) {
      return false;
    }

    // 5.a. Perform an implementation dependent algorithm to map a key to a
    //      corresponding display name.
    JSString* displayName =
        stablePatternChars.isLatin1()
            ? ComputeSingleDisplayName(cx, fmt, dtpg, dnStyle,
                                       stablePatternChars.latin1Range(),
                                       keyValStr)
            : ComputeSingleDisplayName(cx, fmt, dtpg, dnStyle,
                                       stablePatternChars.twoByteRange(),
                                       keyValStr);
    if (!displayName) {
      return false;
    }

    // 5.b. Append the result string to result.
    result->setDenseElement(i, StringValue(displayName));
  }

  // 6. Return result.
  args.rval().setObject(*result);
  return true;
}

bool js::intl_GetLocaleInfo(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);

  UniqueChars locale = intl::EncodeLocale(cx, args[0].toString());
  if (!locale) {
    return false;
  }

  RootedObject info(cx, NewBuiltinClassInstance<PlainObject>(cx));
  if (!info) {
    return false;
  }

  if (!DefineDataProperty(cx, info, cx->names().locale, args[0])) {
    return false;
  }

  bool rtl = uloc_isRightToLeft(IcuLocale(locale.get()));

  RootedValue dir(cx, StringValue(rtl ? cx->names().rtl : cx->names().ltr));

  if (!DefineDataProperty(cx, info, cx->names().direction, dir)) {
    return false;
  }

  args.rval().setObject(*info);
  return true;
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
    JSContext* cx, SupportedLocaleKind kind, HandleLinearString locale,
    HandleLinearString defaultLocale) {
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
  RootedLinearString candidate(cx, locale);

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
    } else {
      MOZ_ASSERT(StringEqualsLiteral(typeStr, "RelativeTimeFormat"));
      kind = SupportedLocaleKind::RelativeTimeFormat;
    }
  }

  RootedLinearString locale(cx, args[1].toString()->ensureLinear(cx));
  if (!locale) {
    return false;
  }

#ifdef DEBUG
  {
    intl::LanguageTag tag(cx);
    bool ok;
    JS_TRY_VAR_OR_RETURN_FALSE(
        cx, ok, intl::LanguageTagParser::tryParse(cx, locale, tag));
    MOZ_ASSERT(ok, "locale is a structurally valid language tag");

    MOZ_ASSERT(!tag.unicodeExtension(),
               "locale must contain no Unicode extensions");

    if (!tag.canonicalize(cx)) {
      return false;
    }

    JSString* tagStr = tag.toString(cx);
    if (!tagStr) {
      return false;
    }

    bool canonical;
    if (!EqualStrings(cx, locale, tagStr, &canonical)) {
      return false;
    }
    MOZ_ASSERT(canonical, "locale is a canonicalized language tag");
  }
#endif

  MOZ_ASSERT(args[2].isNull() || args[2].isString());

  RootedLinearString defaultLocale(cx);
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

  RootedLinearString locale(cx, args[0].toString()->ensureLinear(cx));
  if (!locale) {
    return false;
  }

  intl::LanguageTag tag(cx);
  bool ok;
  JS_TRY_VAR_OR_RETURN_FALSE(
      cx, ok, intl::LanguageTagParser::tryParse(cx, locale, tag));

  RootedLinearString candidate(cx);
  if (!ok) {
    candidate = NewStringCopyZ<CanGC>(cx, intl::LastDitchLocale());
    if (!candidate) {
      return false;
    }
  } else {
    if (!tag.canonicalize(cx)) {
      return false;
    }

    // The default locale must be in [[AvailableLocales]], and that list must
    // not contain any locales with Unicode extension sequences, so remove any
    // present in the candidate.
    tag.clearUnicodeExtension();

    JSString* canonical = tag.toString(cx);
    if (!canonical) {
      return false;
    }

    candidate = canonical->ensureLinear(cx);
    if (!candidate) {
      return false;
    }

    // Certain old-style language tags lack a script code, but in current usage
    // they *would* include a script code. Map these over to modern forms.
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
  // That implies we must ignore any candidate which isn't supported by all Intl
  // service constructors.

  RootedLinearString supportedCollator(cx);
  JS_TRY_VAR_OR_RETURN_FALSE(
      cx, supportedCollator,
      BestAvailableLocale(cx, SupportedLocaleKind::Collator, candidate,
                          nullptr));

  RootedLinearString supportedDateTimeFormat(cx);
  JS_TRY_VAR_OR_RETURN_FALSE(
      cx, supportedDateTimeFormat,
      BestAvailableLocale(cx, SupportedLocaleKind::DateTimeFormat, candidate,
                          nullptr));

#ifdef DEBUG
  // Note: We don't test the supported locales of the remaining Intl service
  // constructors, because the set of supported locales is exactly equal to the
  // set of supported locales of Intl.DateTimeFormat.
  for (auto kind :
       {SupportedLocaleKind::DisplayNames, SupportedLocaleKind::ListFormat,
        SupportedLocaleKind::NumberFormat, SupportedLocaleKind::PluralRules,
        SupportedLocaleKind::RelativeTimeFormat}) {
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

static bool intl_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().Intl);
  return true;
}

static const JSFunctionSpec intl_static_methods[] = {
    JS_FN(js_toSource_str, intl_toSource, 0, 0),
    JS_SELF_HOSTED_FN("getCanonicalLocales", "Intl_getCanonicalLocales", 1, 0),
    JS_FS_END};

static const JSPropertySpec intl_static_properties[] = {
    JS_STRING_SYM_PS(toStringTag, "Intl", JSPROP_READONLY), JS_PS_END};

static JSObject* CreateIntlObject(JSContext* cx, JSProtoKey key) {
  Handle<GlobalObject*> global = cx->global();
  RootedObject proto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));
  if (!proto) {
    return nullptr;
  }

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
  for (const auto& protoKey :
       {JSProto_Collator, JSProto_DateTimeFormat, JSProto_DisplayNames,
        JSProto_ListFormat, JSProto_Locale, JSProto_NumberFormat,
        JSProto_PluralRules, JSProto_RelativeTimeFormat}) {
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
