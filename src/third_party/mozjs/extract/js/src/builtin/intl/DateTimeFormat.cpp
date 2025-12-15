/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Intl.DateTimeFormat implementation. */

#include "builtin/intl/DateTimeFormat.h"

#include "mozilla/Assertions.h"
#include "mozilla/intl/Calendar.h"
#include "mozilla/intl/DateIntervalFormat.h"
#include "mozilla/intl/DateTimeFormat.h"
#include "mozilla/intl/DateTimePart.h"
#include "mozilla/intl/Locale.h"
#include "mozilla/intl/TimeZone.h"
#include "mozilla/Range.h"
#include "mozilla/Span.h"

#include "jsdate.h"

#include "builtin/Array.h"
#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/FormatBuffer.h"
#include "builtin/intl/LanguageTag.h"
#include "builtin/intl/SharedIntlData.h"
#include "builtin/temporal/Calendar.h"
#include "builtin/temporal/Instant.h"
#include "builtin/temporal/PlainDate.h"
#include "builtin/temporal/PlainDateTime.h"
#include "builtin/temporal/PlainMonthDay.h"
#include "builtin/temporal/PlainTime.h"
#include "builtin/temporal/PlainYearMonth.h"
#include "builtin/temporal/Temporal.h"
#include "builtin/temporal/TemporalParser.h"
#include "builtin/temporal/TimeZone.h"
#include "builtin/temporal/ZonedDateTime.h"
#include "gc/GCContext.h"
#include "js/Date.h"
#include "js/experimental/Intl.h"     // JS::AddMozDateTimeFormatConstructor
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/GCAPI.h"
#include "js/PropertyAndElement.h"  // JS_DefineFunctions, JS_DefineProperties
#include "js/PropertySpec.h"
#include "js/StableStringChars.h"
#include "js/Wrapper.h"
#include "vm/DateTime.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/Runtime.h"

#include "vm/GeckoProfiler-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;
using namespace js::temporal;

using JS::AutoStableStringChars;
using JS::ClippedTime;
using JS::TimeClip;

using js::intl::DateTimeFormatOptions;
using js::intl::FormatBuffer;
using js::intl::INITIAL_CHAR_BUFFER_SIZE;
using js::intl::SharedIntlData;

const JSClassOps DateTimeFormatObject::classOps_ = {
    nullptr,                         // addProperty
    nullptr,                         // delProperty
    nullptr,                         // enumerate
    nullptr,                         // newEnumerate
    nullptr,                         // resolve
    nullptr,                         // mayResolve
    DateTimeFormatObject::finalize,  // finalize
    nullptr,                         // call
    nullptr,                         // construct
    nullptr,                         // trace
};

const JSClass DateTimeFormatObject::class_ = {
    "Intl.DateTimeFormat",
    JSCLASS_HAS_RESERVED_SLOTS(DateTimeFormatObject::SLOT_COUNT) |
        JSCLASS_HAS_CACHED_PROTO(JSProto_DateTimeFormat) |
        JSCLASS_FOREGROUND_FINALIZE,
    &DateTimeFormatObject::classOps_,
    &DateTimeFormatObject::classSpec_,
};

const JSClass& DateTimeFormatObject::protoClass_ = PlainObject::class_;

static bool dateTimeFormat_toSource(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  args.rval().setString(cx->names().DateTimeFormat);
  return true;
}

static const JSFunctionSpec dateTimeFormat_static_methods[] = {
    JS_SELF_HOSTED_FN("supportedLocalesOf",
                      "Intl_DateTimeFormat_supportedLocalesOf", 1, 0),
    JS_FS_END,
};

static const JSFunctionSpec dateTimeFormat_methods[] = {
    JS_SELF_HOSTED_FN("resolvedOptions", "Intl_DateTimeFormat_resolvedOptions",
                      0, 0),
    JS_SELF_HOSTED_FN("formatToParts", "Intl_DateTimeFormat_formatToParts", 1,
                      0),
    JS_SELF_HOSTED_FN("formatRange", "Intl_DateTimeFormat_formatRange", 2, 0),
    JS_SELF_HOSTED_FN("formatRangeToParts",
                      "Intl_DateTimeFormat_formatRangeToParts", 2, 0),
    JS_FN("toSource", dateTimeFormat_toSource, 0, 0),
    JS_FS_END,
};

static const JSPropertySpec dateTimeFormat_properties[] = {
    JS_SELF_HOSTED_GET("format", "$Intl_DateTimeFormat_format_get", 0),
    JS_STRING_SYM_PS(toStringTag, "Intl.DateTimeFormat", JSPROP_READONLY),
    JS_PS_END,
};

static bool DateTimeFormat(JSContext* cx, unsigned argc, Value* vp);

const ClassSpec DateTimeFormatObject::classSpec_ = {
    GenericCreateConstructor<DateTimeFormat, 0, gc::AllocKind::FUNCTION>,
    GenericCreatePrototype<DateTimeFormatObject>,
    dateTimeFormat_static_methods,
    nullptr,
    dateTimeFormat_methods,
    dateTimeFormat_properties,
    nullptr,
    ClassSpec::DontDefineConstructor,
};

/**
 * 12.2.1 Intl.DateTimeFormat([ locales [, options]])
 *
 * ES2017 Intl draft rev 94045d234762ad107a3d09bb6f7381a65f1a2f9b
 */
static bool DateTimeFormat(JSContext* cx, const CallArgs& args, bool construct,
                           HandleString required, HandleString defaults,
                           DateTimeFormatOptions dtfOptions) {
  AutoJSConstructorProfilerEntry pseudoFrame(cx, "Intl.DateTimeFormat");

  // Step 1 (Handled by OrdinaryCreateFromConstructor fallback code).

  // Step 2 (Inlined 9.1.14, OrdinaryCreateFromConstructor).
  JSProtoKey protoKey = dtfOptions == DateTimeFormatOptions::Standard
                            ? JSProto_DateTimeFormat
                            : JSProto_Null;
  RootedObject proto(cx);
  if (!GetPrototypeFromBuiltinConstructor(cx, args, protoKey, &proto)) {
    return false;
  }

  Rooted<DateTimeFormatObject*> dateTimeFormat(cx);
  dateTimeFormat = NewObjectWithClassProto<DateTimeFormatObject>(cx, proto);
  if (!dateTimeFormat) {
    return false;
  }

  RootedValue thisValue(
      cx, construct ? ObjectValue(*dateTimeFormat) : args.thisv());
  HandleValue locales = args.get(0);
  HandleValue options = args.get(1);

  // Step 3.
  return intl::InitializeDateTimeFormatObject(
      cx, dateTimeFormat, thisValue, locales, options, required, defaults,
      UndefinedHandleValue, dtfOptions, args.rval());
}

static bool DateTimeFormat(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  Handle<PropertyName*> required = cx->names().any;
  Handle<PropertyName*> defaults = cx->names().date;
  return DateTimeFormat(cx, args, args.isConstructing(), required, defaults,
                        DateTimeFormatOptions::Standard);
}

static bool MozDateTimeFormat(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  // Don't allow to call mozIntl.DateTimeFormat as a function. That way we
  // don't need to worry how to handle the legacy initialization semantics
  // when applied on mozIntl.DateTimeFormat.
  if (!ThrowIfNotConstructing(cx, args, "mozIntl.DateTimeFormat")) {
    return false;
  }

  Handle<PropertyName*> required = cx->names().any;
  Handle<PropertyName*> defaults = cx->names().date;
  return DateTimeFormat(cx, args, true, required, defaults,
                        DateTimeFormatOptions::EnableMozExtensions);
}

bool js::intl_CreateDateTimeFormat(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 4);
  MOZ_ASSERT(!args.isConstructing());

  RootedString required(cx, args[2].toString());
  RootedString defaults(cx, args[3].toString());

  // intl_CreateDateTimeFormat is an intrinsic for self-hosted JavaScript, so it
  // cannot be used with "new", but it still has to be treated as a constructor.
  return DateTimeFormat(cx, args, true, required, defaults,
                        DateTimeFormatOptions::Standard);
}

void js::DateTimeFormatObject::finalize(JS::GCContext* gcx, JSObject* obj) {
  MOZ_ASSERT(gcx->onMainThread());

  auto* dateTimeFormat = &obj->as<DateTimeFormatObject>();
  mozilla::intl::DateTimeFormat* df = dateTimeFormat->getDateFormat();
  mozilla::intl::DateIntervalFormat* dif =
      dateTimeFormat->getDateIntervalFormat();

  if (df) {
    intl::RemoveICUCellMemory(
        gcx, obj, DateTimeFormatObject::UDateFormatEstimatedMemoryUse);

    delete df;
  }

  if (dif) {
    intl::RemoveICUCellMemory(
        gcx, obj, DateTimeFormatObject::UDateIntervalFormatEstimatedMemoryUse);

    delete dif;
  }
}

bool JS::AddMozDateTimeFormatConstructor(JSContext* cx,
                                         JS::Handle<JSObject*> intl) {
  RootedObject ctor(
      cx, GlobalObject::createConstructor(cx, MozDateTimeFormat,
                                          cx->names().DateTimeFormat, 0));
  if (!ctor) {
    return false;
  }

  RootedObject proto(
      cx, GlobalObject::createBlankPrototype<PlainObject>(cx, cx->global()));
  if (!proto) {
    return false;
  }

  if (!LinkConstructorAndPrototype(cx, ctor, proto)) {
    return false;
  }

  // 12.3.2
  if (!JS_DefineFunctions(cx, ctor, dateTimeFormat_static_methods)) {
    return false;
  }

  // 12.4.4 and 12.4.5
  if (!JS_DefineFunctions(cx, proto, dateTimeFormat_methods)) {
    return false;
  }

  // 12.4.2 and 12.4.3
  if (!JS_DefineProperties(cx, proto, dateTimeFormat_properties)) {
    return false;
  }

  RootedValue ctorValue(cx, ObjectValue(*ctor));
  return DefineDataProperty(cx, intl, cx->names().DateTimeFormat, ctorValue, 0);
}

static bool DefaultCalendar(JSContext* cx, const UniqueChars& locale,
                            MutableHandleValue rval) {
  auto calendar = mozilla::intl::Calendar::TryCreate(locale.get());
  if (calendar.isErr()) {
    intl::ReportInternalError(cx, calendar.unwrapErr());
    return false;
  }

  auto type = calendar.unwrap()->GetBcp47Type();
  if (type.isErr()) {
    intl::ReportInternalError(cx, type.unwrapErr());
    return false;
  }

  JSString* str = NewStringCopy<CanGC>(cx, type.unwrap());
  if (!str) {
    return false;
  }

  rval.setString(str);
  return true;
}

bool js::intl_availableCalendars(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isString());

  UniqueChars locale = intl::EncodeLocale(cx, args[0].toString());
  if (!locale) {
    return false;
  }

  RootedObject calendars(cx, NewDenseEmptyArray(cx));
  if (!calendars) {
    return false;
  }

  // We need the default calendar for the locale as the first result.
  RootedValue defaultCalendar(cx);
  if (!DefaultCalendar(cx, locale, &defaultCalendar)) {
    return false;
  }

  if (!NewbornArrayPush(cx, calendars, defaultCalendar)) {
    return false;
  }

  // Now get the calendars that "would make a difference", i.e., not the
  // default.
  auto keywords =
      mozilla::intl::Calendar::GetBcp47KeywordValuesForLocale(locale.get());
  if (keywords.isErr()) {
    intl::ReportInternalError(cx, keywords.unwrapErr());
    return false;
  }

  for (auto keyword : keywords.unwrap()) {
    if (keyword.isErr()) {
      intl::ReportInternalError(cx);
      return false;
    }

    JSString* jscalendar = NewStringCopy<CanGC>(cx, keyword.unwrap());
    if (!jscalendar) {
      return false;
    }
    if (!NewbornArrayPush(cx, calendars, StringValue(jscalendar))) {
      return false;
    }
  }

  args.rval().setObject(*calendars);
  return true;
}

bool js::intl_defaultCalendar(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isString());

  UniqueChars locale = intl::EncodeLocale(cx, args[0].toString());
  if (!locale) {
    return false;
  }

  return DefaultCalendar(cx, locale, args.rval());
}

bool js::intl_IsValidTimeZoneName(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isString());

  SharedIntlData& sharedIntlData = cx->runtime()->sharedIntlData.ref();

  RootedString timeZone(cx, args[0].toString());
  Rooted<JSAtom*> validatedTimeZone(cx);
  if (!sharedIntlData.validateTimeZoneName(cx, timeZone, &validatedTimeZone)) {
    return false;
  }

  if (validatedTimeZone) {
    cx->markAtom(validatedTimeZone);
    args.rval().setString(validatedTimeZone);
  } else {
    args.rval().setNull();
  }
  return true;
}

JSLinearString* js::intl::CanonicalizeTimeZone(JSContext* cx,
                                               Handle<JSString*> timeZone) {
  SharedIntlData& sharedIntlData = cx->runtime()->sharedIntlData.ref();

  // Some time zone names are canonicalized differently by ICU -- handle those
  // first.
  Rooted<JSAtom*> ianaTimeZone(cx);
  if (!sharedIntlData.tryCanonicalizeTimeZoneConsistentWithIANA(
          cx, timeZone, &ianaTimeZone)) {
    return nullptr;
  }

  JSLinearString* resultTimeZone;
  if (ianaTimeZone) {
    cx->markAtom(ianaTimeZone);
    resultTimeZone = ianaTimeZone;
  } else {
    AutoStableStringChars stableChars(cx);
    if (!stableChars.initTwoByte(cx, timeZone)) {
      return nullptr;
    }

    // Call into ICU to canonicalize the time zone.
    FormatBuffer<char16_t, INITIAL_CHAR_BUFFER_SIZE> canonicalTimeZone(cx);
    auto result = mozilla::intl::TimeZone::GetCanonicalTimeZoneID(
        stableChars.twoByteRange(), canonicalTimeZone);
    if (result.isErr()) {
      ReportInternalError(cx, result.unwrapErr());
      return nullptr;
    }

    resultTimeZone = canonicalTimeZone.toString(cx);
    if (!resultTimeZone) {
      return nullptr;
    }
  }

  MOZ_ASSERT(!StringEqualsLiteral(resultTimeZone, "Etc/Unknown"),
             "Invalid canonical time zone");

  // Links to UTC are handled by SharedIntlData.
  MOZ_ASSERT(!StringEqualsLiteral(resultTimeZone, "GMT"));
  MOZ_ASSERT(!StringEqualsLiteral(resultTimeZone, "Etc/UTC"));
  MOZ_ASSERT(!StringEqualsLiteral(resultTimeZone, "Etc/GMT"));

  return resultTimeZone;
}

bool js::intl_canonicalizeTimeZone(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isString());

  RootedString timeZone(cx, args[0].toString());
  auto* result = intl::CanonicalizeTimeZone(cx, timeZone);
  if (!result) {
    return false;
  }

  args.rval().setString(result);
  return true;
}

bool js::intl_defaultTimeZone(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  FormatBuffer<char16_t, intl::INITIAL_CHAR_BUFFER_SIZE> timeZone(cx);
  auto result =
      DateTimeInfo::timeZoneId(DateTimeInfo::forceUTC(cx->realm()), timeZone);
  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return false;
  }

  JSString* str = timeZone.toString(cx);
  if (!str) {
    return false;
  }

  args.rval().setString(str);
  return true;
}

bool js::intl_defaultTimeZoneOffset(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 0);

  auto offset =
      DateTimeInfo::getRawOffsetMs(DateTimeInfo::forceUTC(cx->realm()));
  if (offset.isErr()) {
    intl::ReportInternalError(cx, offset.unwrapErr());
    return false;
  }

  args.rval().setInt32(offset.unwrap());
  return true;
}

bool js::intl_isDefaultTimeZone(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 1);
  MOZ_ASSERT(args[0].isString() || args[0].isUndefined());

  // |undefined| is the default value when the Intl runtime caches haven't
  // yet been initialized. Handle it the same way as a cache miss.
  if (args[0].isUndefined()) {
    args.rval().setBoolean(false);
    return true;
  }

  FormatBuffer<char16_t, intl::INITIAL_CHAR_BUFFER_SIZE> chars(cx);
  auto result =
      DateTimeInfo::timeZoneId(DateTimeInfo::forceUTC(cx->realm()), chars);
  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return false;
  }

  JSLinearString* str = args[0].toString()->ensureLinear(cx);
  if (!str) {
    return false;
  }

  bool equals;
  if (str->length() == chars.length()) {
    JS::AutoCheckCannotGC nogc;
    equals =
        str->hasLatin1Chars()
            ? EqualChars(str->latin1Chars(nogc), chars.data(), str->length())
            : EqualChars(str->twoByteChars(nogc), chars.data(), str->length());
  } else {
    equals = false;
  }

  args.rval().setBoolean(equals);
  return true;
}

enum class HourCycle {
  // 12 hour cycle, from 0 to 11.
  H11,

  // 12 hour cycle, from 1 to 12.
  H12,

  // 24 hour cycle, from 0 to 23.
  H23,

  // 24 hour cycle, from 1 to 24.
  H24
};

static UniqueChars DateTimeFormatLocale(
    JSContext* cx, HandleObject internals,
    mozilla::Maybe<mozilla::intl::DateTimeFormat::HourCycle> hourCycle =
        mozilla::Nothing()) {
  RootedValue value(cx);
  if (!GetProperty(cx, internals, internals, cx->names().locale, &value)) {
    return nullptr;
  }

  // ICU expects calendar, numberingSystem, and hourCycle as Unicode locale
  // extensions on locale.

  mozilla::intl::Locale tag;
  {
    Rooted<JSLinearString*> locale(cx, value.toString()->ensureLinear(cx));
    if (!locale) {
      return nullptr;
    }

    if (!intl::ParseLocale(cx, locale, tag)) {
      return nullptr;
    }
  }

  JS::RootedVector<intl::UnicodeExtensionKeyword> keywords(cx);

  if (!GetProperty(cx, internals, internals, cx->names().calendar, &value)) {
    return nullptr;
  }

  {
    JSLinearString* calendar = value.toString()->ensureLinear(cx);
    if (!calendar) {
      return nullptr;
    }

    if (!keywords.emplaceBack("ca", calendar)) {
      return nullptr;
    }
  }

  if (!GetProperty(cx, internals, internals, cx->names().numberingSystem,
                   &value)) {
    return nullptr;
  }

  {
    JSLinearString* numberingSystem = value.toString()->ensureLinear(cx);
    if (!numberingSystem) {
      return nullptr;
    }

    if (!keywords.emplaceBack("nu", numberingSystem)) {
      return nullptr;
    }
  }

  if (hourCycle) {
    JSAtom* hourCycleStr;
    switch (*hourCycle) {
      case mozilla::intl::DateTimeFormat::HourCycle::H11:
        hourCycleStr = cx->names().h11;
        break;
      case mozilla::intl::DateTimeFormat::HourCycle::H12:
        hourCycleStr = cx->names().h12;
        break;
      case mozilla::intl::DateTimeFormat::HourCycle::H23:
        hourCycleStr = cx->names().h23;
        break;
      case mozilla::intl::DateTimeFormat::HourCycle::H24:
        hourCycleStr = cx->names().h24;
        break;
    }

    if (!keywords.emplaceBack("hc", hourCycleStr)) {
      return nullptr;
    }
  }

  // |ApplyUnicodeExtensionToTag| applies the new keywords to the front of
  // the Unicode extension subtag. We're then relying on ICU to follow RFC
  // 6067, which states that any trailing keywords using the same key
  // should be ignored.
  if (!intl::ApplyUnicodeExtensionToTag(cx, tag, keywords)) {
    return nullptr;
  }

  FormatBuffer<char> buffer(cx);
  if (auto result = tag.ToString(buffer); result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return nullptr;
  }
  return buffer.extractStringZ();
}

static bool AssignTextComponent(
    JSContext* cx, HandleObject internals, Handle<PropertyName*> property,
    mozilla::Maybe<mozilla::intl::DateTimeFormat::Text>* text) {
  RootedValue value(cx);
  if (!GetProperty(cx, internals, internals, property, &value)) {
    return false;
  }

  if (value.isString()) {
    JSLinearString* string = value.toString()->ensureLinear(cx);
    if (!string) {
      return false;
    }
    if (StringEqualsLiteral(string, "narrow")) {
      *text = mozilla::Some(mozilla::intl::DateTimeFormat::Text::Narrow);
    } else if (StringEqualsLiteral(string, "short")) {
      *text = mozilla::Some(mozilla::intl::DateTimeFormat::Text::Short);
    } else {
      MOZ_ASSERT(StringEqualsLiteral(string, "long"));
      *text = mozilla::Some(mozilla::intl::DateTimeFormat::Text::Long);
    }
  } else {
    MOZ_ASSERT(value.isUndefined());
  }

  return true;
}

static bool AssignNumericComponent(
    JSContext* cx, HandleObject internals, Handle<PropertyName*> property,
    mozilla::Maybe<mozilla::intl::DateTimeFormat::Numeric>* numeric) {
  RootedValue value(cx);
  if (!GetProperty(cx, internals, internals, property, &value)) {
    return false;
  }

  if (value.isString()) {
    JSLinearString* string = value.toString()->ensureLinear(cx);
    if (!string) {
      return false;
    }
    if (StringEqualsLiteral(string, "numeric")) {
      *numeric = mozilla::Some(mozilla::intl::DateTimeFormat::Numeric::Numeric);
    } else {
      MOZ_ASSERT(StringEqualsLiteral(string, "2-digit"));
      *numeric =
          mozilla::Some(mozilla::intl::DateTimeFormat::Numeric::TwoDigit);
    }
  } else {
    MOZ_ASSERT(value.isUndefined());
  }

  return true;
}

static bool AssignMonthComponent(
    JSContext* cx, HandleObject internals, Handle<PropertyName*> property,
    mozilla::Maybe<mozilla::intl::DateTimeFormat::Month>* month) {
  RootedValue value(cx);
  if (!GetProperty(cx, internals, internals, property, &value)) {
    return false;
  }

  if (value.isString()) {
    JSLinearString* string = value.toString()->ensureLinear(cx);
    if (!string) {
      return false;
    }
    if (StringEqualsLiteral(string, "numeric")) {
      *month = mozilla::Some(mozilla::intl::DateTimeFormat::Month::Numeric);
    } else if (StringEqualsLiteral(string, "2-digit")) {
      *month = mozilla::Some(mozilla::intl::DateTimeFormat::Month::TwoDigit);
    } else if (StringEqualsLiteral(string, "long")) {
      *month = mozilla::Some(mozilla::intl::DateTimeFormat::Month::Long);
    } else if (StringEqualsLiteral(string, "short")) {
      *month = mozilla::Some(mozilla::intl::DateTimeFormat::Month::Short);
    } else {
      MOZ_ASSERT(StringEqualsLiteral(string, "narrow"));
      *month = mozilla::Some(mozilla::intl::DateTimeFormat::Month::Narrow);
    }
  } else {
    MOZ_ASSERT(value.isUndefined());
  }

  return true;
}

static bool AssignTimeZoneNameComponent(
    JSContext* cx, HandleObject internals, Handle<PropertyName*> property,
    mozilla::Maybe<mozilla::intl::DateTimeFormat::TimeZoneName>* tzName) {
  RootedValue value(cx);
  if (!GetProperty(cx, internals, internals, property, &value)) {
    return false;
  }

  if (value.isString()) {
    JSLinearString* string = value.toString()->ensureLinear(cx);
    if (!string) {
      return false;
    }
    if (StringEqualsLiteral(string, "long")) {
      *tzName =
          mozilla::Some(mozilla::intl::DateTimeFormat::TimeZoneName::Long);
    } else if (StringEqualsLiteral(string, "short")) {
      *tzName =
          mozilla::Some(mozilla::intl::DateTimeFormat::TimeZoneName::Short);
    } else if (StringEqualsLiteral(string, "shortOffset")) {
      *tzName = mozilla::Some(
          mozilla::intl::DateTimeFormat::TimeZoneName::ShortOffset);
    } else if (StringEqualsLiteral(string, "longOffset")) {
      *tzName = mozilla::Some(
          mozilla::intl::DateTimeFormat::TimeZoneName::LongOffset);
    } else if (StringEqualsLiteral(string, "shortGeneric")) {
      *tzName = mozilla::Some(
          mozilla::intl::DateTimeFormat::TimeZoneName::ShortGeneric);
    } else {
      MOZ_ASSERT(StringEqualsLiteral(string, "longGeneric"));
      *tzName = mozilla::Some(
          mozilla::intl::DateTimeFormat::TimeZoneName::LongGeneric);
    }
  } else {
    MOZ_ASSERT(value.isUndefined());
  }

  return true;
}

static bool AssignHourCycleComponent(
    JSContext* cx, HandleObject internals, Handle<PropertyName*> property,
    mozilla::Maybe<mozilla::intl::DateTimeFormat::HourCycle>* hourCycle) {
  RootedValue value(cx);
  if (!GetProperty(cx, internals, internals, property, &value)) {
    return false;
  }

  if (value.isString()) {
    JSLinearString* string = value.toString()->ensureLinear(cx);
    if (!string) {
      return false;
    }
    if (StringEqualsLiteral(string, "h11")) {
      *hourCycle = mozilla::Some(mozilla::intl::DateTimeFormat::HourCycle::H11);
    } else if (StringEqualsLiteral(string, "h12")) {
      *hourCycle = mozilla::Some(mozilla::intl::DateTimeFormat::HourCycle::H12);
    } else if (StringEqualsLiteral(string, "h23")) {
      *hourCycle = mozilla::Some(mozilla::intl::DateTimeFormat::HourCycle::H23);
    } else {
      MOZ_ASSERT(StringEqualsLiteral(string, "h24"));
      *hourCycle = mozilla::Some(mozilla::intl::DateTimeFormat::HourCycle::H24);
    }
  } else {
    MOZ_ASSERT(value.isUndefined());
  }

  return true;
}

static bool AssignHour12Component(JSContext* cx, HandleObject internals,
                                  mozilla::Maybe<bool>* hour12) {
  RootedValue value(cx);
  if (!GetProperty(cx, internals, internals, cx->names().hour12, &value)) {
    return false;
  }
  if (value.isBoolean()) {
    *hour12 = mozilla::Some(value.toBoolean());
  } else {
    MOZ_ASSERT(value.isUndefined());
  }

  return true;
}

static bool AssignDateTimeLength(
    JSContext* cx, HandleObject internals, Handle<PropertyName*> property,
    mozilla::Maybe<mozilla::intl::DateTimeFormat::Style>* style) {
  RootedValue value(cx);
  if (!GetProperty(cx, internals, internals, property, &value)) {
    return false;
  }

  if (value.isString()) {
    JSLinearString* string = value.toString()->ensureLinear(cx);
    if (!string) {
      return false;
    }
    if (StringEqualsLiteral(string, "full")) {
      *style = mozilla::Some(mozilla::intl::DateTimeFormat::Style::Full);
    } else if (StringEqualsLiteral(string, "long")) {
      *style = mozilla::Some(mozilla::intl::DateTimeFormat::Style::Long);
    } else if (StringEqualsLiteral(string, "medium")) {
      *style = mozilla::Some(mozilla::intl::DateTimeFormat::Style::Medium);
    } else {
      MOZ_ASSERT(StringEqualsLiteral(string, "short"));
      *style = mozilla::Some(mozilla::intl::DateTimeFormat::Style::Short);
    }
  } else {
    MOZ_ASSERT(value.isUndefined());
  }

  return true;
}

enum class Required { Date, Time, YearMonth, MonthDay, Any };

enum class Defaults { Date, Time, YearMonth, MonthDay, ZonedDateTime, All };

enum class Inherit { All, Relevant };

struct DateTimeFormatArgs {
  Required required;
  Defaults defaults;
  Inherit inherit;
};

/**
 * Get the "required" argument passed to CreateDateTimeFormat.
 */
static bool GetRequired(JSContext* cx, Handle<JSObject*> internals,
                        Required* result) {
  Rooted<Value> value(cx);
  if (!GetProperty(cx, internals, internals, cx->names().required, &value)) {
    return false;
  }
  MOZ_ASSERT(value.isString());

  JSLinearString* string = value.toString()->ensureLinear(cx);
  if (!string) {
    return false;
  }

  if (StringEqualsLiteral(string, "date")) {
    *result = Required::Date;
  } else if (StringEqualsLiteral(string, "time")) {
    *result = Required::Time;
  } else {
    MOZ_ASSERT(StringEqualsLiteral(string, "any"));
    *result = Required::Any;
  }
  return true;
}

/**
 * Get the "defaults" argument passed to CreateDateTimeFormat.
 */
static bool GetDefaults(JSContext* cx, Handle<JSObject*> internals,
                        Defaults* result) {
  Rooted<Value> value(cx);
  if (!GetProperty(cx, internals, internals, cx->names().defaults, &value)) {
    return false;
  }
  MOZ_ASSERT(value.isString());

  JSLinearString* string = value.toString()->ensureLinear(cx);
  if (!string) {
    return false;
  }

  if (StringEqualsLiteral(string, "date")) {
    *result = Defaults::Date;
  } else if (StringEqualsLiteral(string, "time")) {
    *result = Defaults::Time;
  } else {
    MOZ_ASSERT(StringEqualsLiteral(string, "all"));
    *result = Defaults::All;
  }
  return true;
}

/**
 * Compute the (required, defaults, inherit) arguments passed to
 * GetDateTimeFormat.
 */
static bool GetDateTimeFormatArgs(JSContext* cx, Handle<JSObject*> internals,
                                  DateTimeValueKind kind,
                                  DateTimeFormatArgs* result) {
  switch (kind) {
    case DateTimeValueKind::Number: {
      Required required;
      if (!GetRequired(cx, internals, &required)) {
        return false;
      }
      Defaults defaults;
      if (!GetDefaults(cx, internals, &defaults)) {
        return false;
      }
      *result = {required, defaults, Inherit::All};
      return true;
    }
    case DateTimeValueKind::TemporalDate:
      *result = {Required::Date, Defaults::Date, Inherit::Relevant};
      return true;
    case DateTimeValueKind::TemporalTime:
      *result = {Required::Time, Defaults::Time, Inherit::Relevant};
      return true;
    case DateTimeValueKind::TemporalDateTime:
      *result = {Required::Any, Defaults::All, Inherit::Relevant};
      return true;
    case DateTimeValueKind::TemporalYearMonth:
      *result = {Required::YearMonth, Defaults::YearMonth, Inherit::Relevant};
      return true;
    case DateTimeValueKind::TemporalMonthDay:
      *result = {Required::MonthDay, Defaults::MonthDay, Inherit::Relevant};
      return true;
    case DateTimeValueKind::TemporalZonedDateTime:
      *result = {Required::Any, Defaults::ZonedDateTime, Inherit::All};
      return true;
    case DateTimeValueKind::TemporalInstant:
      *result = {Required::Any, Defaults::All, Inherit::All};
      return true;
  }
  MOZ_CRASH("invalid date-time value kind");
}

enum class DateTimeField {
  Weekday,
  Era,
  Year,
  Month,
  Day,
  DayPeriod,
  Hour,
  Minute,
  Second,
  FractionalSecondDigits,
};

/**
 * GetDateTimeFormat ( formats, matcher, options, required, defaults, inherit )
 *
 * https://tc39.es/proposal-temporal/#sec-getdatetimeformat
 */
static mozilla::Maybe<mozilla::intl::DateTimeFormat::ComponentsBag>
GetDateTimeFormat(const mozilla::intl::DateTimeFormat::ComponentsBag& options,
                  Required required, Defaults defaults, Inherit inherit) {
  // Steps 1-5.
  mozilla::EnumSet<DateTimeField> requiredOptions;
  switch (required) {
    case Required::Date:
      requiredOptions = {
          DateTimeField::Weekday,
          DateTimeField::Year,
          DateTimeField::Month,
          DateTimeField::Day,
      };
      break;
    case Required::Time:
      requiredOptions = {
          DateTimeField::DayPeriod,
          DateTimeField::Hour,
          DateTimeField::Minute,
          DateTimeField::Second,
          DateTimeField::FractionalSecondDigits,
      };
      break;
    case Required::YearMonth:
      requiredOptions = {
          DateTimeField::Year,
          DateTimeField::Month,
      };
      break;
    case Required::MonthDay:
      requiredOptions = {
          DateTimeField::Month,
          DateTimeField::Day,
      };
      break;
    case Required::Any:
      requiredOptions = {
          DateTimeField::Weekday,
          DateTimeField::Year,
          DateTimeField::Month,
          DateTimeField::Day,
          DateTimeField::DayPeriod,
          DateTimeField::Hour,
          DateTimeField::Minute,
          DateTimeField::Second,
          DateTimeField::FractionalSecondDigits,
      };
      break;
  }
  MOZ_ASSERT(!requiredOptions.contains(DateTimeField::Era),
             "standalone era not supported");

  // Steps 6-10.
  mozilla::EnumSet<DateTimeField> defaultOptions;
  switch (defaults) {
    case Defaults::Date:
      defaultOptions = {
          DateTimeField::Year,
          DateTimeField::Month,
          DateTimeField::Day,
      };
      break;
    case Defaults::Time:
      defaultOptions = {
          DateTimeField::Hour,
          DateTimeField::Minute,
          DateTimeField::Second,
      };
      break;
    case Defaults::YearMonth:
      defaultOptions = {
          DateTimeField::Year,
          DateTimeField::Month,
      };
      break;
    case Defaults::MonthDay:
      defaultOptions = {
          DateTimeField::Month,
          DateTimeField::Day,
      };
      break;
    case Defaults::ZonedDateTime:
    case Defaults::All:
      defaultOptions = {
          DateTimeField::Year, DateTimeField::Month,  DateTimeField::Day,
          DateTimeField::Hour, DateTimeField::Minute, DateTimeField::Second,
      };
      break;
  }
  MOZ_ASSERT(!defaultOptions.contains(DateTimeField::Weekday));
  MOZ_ASSERT(!defaultOptions.contains(DateTimeField::Era));
  MOZ_ASSERT(!defaultOptions.contains(DateTimeField::DayPeriod));
  MOZ_ASSERT(!defaultOptions.contains(DateTimeField::FractionalSecondDigits));

  // Steps 11-12.
  mozilla::intl::DateTimeFormat::ComponentsBag formatOptions;
  if (inherit == Inherit::All) {
    // Step 11.a.
    formatOptions = options;
  } else {
    // Step 12.a. (Implicit)

    // Step 12.b.
    switch (required) {
      case Required::Date:
      case Required::YearMonth:
      case Required::Any:
        formatOptions.era = options.era;
        break;
      case Required::Time:
      case Required::MonthDay:
        // |era| option not applicable for these types.
        break;
    }

    // Step 12.c.
    switch (required) {
      case Required::Time:
      case Required::Any:
        formatOptions.hourCycle = options.hourCycle;
        formatOptions.hour12 = options.hour12;
        break;
      case Required::Date:
      case Required::YearMonth:
      case Required::MonthDay:
        // |hourCycle| and |hour12| options not applicable for these types.
        break;
    }
  }

  // Steps 13-14.
  //
  // Ignore "era" to workaround a spec bug.
  //
  // FIXME: spec bug - https://github.com/tc39/proposal-temporal/issues/3049
  bool anyPresent = options.weekday || options.year || options.month ||
                    options.day || options.dayPeriod || options.hour ||
                    options.minute || options.second ||
                    options.fractionalSecondDigits;

  // Step 15.
  bool needDefaults = true;

  // Step 16. (Loop unrolled)
  if (requiredOptions.contains(DateTimeField::Weekday) && options.weekday) {
    formatOptions.weekday = options.weekday;
    needDefaults = false;
  }
  if (requiredOptions.contains(DateTimeField::Year) && options.year) {
    formatOptions.year = options.year;
    needDefaults = false;
  }
  if (requiredOptions.contains(DateTimeField::Month) && options.month) {
    formatOptions.month = options.month;
    needDefaults = false;
  }
  if (requiredOptions.contains(DateTimeField::Day) && options.day) {
    formatOptions.day = options.day;
    needDefaults = false;
  }
  if (requiredOptions.contains(DateTimeField::DayPeriod) && options.dayPeriod) {
    formatOptions.dayPeriod = options.dayPeriod;
    needDefaults = false;
  }
  if (requiredOptions.contains(DateTimeField::Hour) && options.hour) {
    formatOptions.hour = options.hour;
    needDefaults = false;
  }
  if (requiredOptions.contains(DateTimeField::Minute) && options.minute) {
    formatOptions.minute = options.minute;
    needDefaults = false;
  }
  if (requiredOptions.contains(DateTimeField::Second) && options.second) {
    formatOptions.second = options.second;
    needDefaults = false;
  }
  if (requiredOptions.contains(DateTimeField::FractionalSecondDigits) &&
      options.fractionalSecondDigits) {
    formatOptions.fractionalSecondDigits = options.fractionalSecondDigits;
    needDefaults = false;
  }

  // Step 17.
  if (needDefaults) {
    // Step 17.a.
    if (anyPresent && inherit == Inherit::Relevant) {
      return mozilla::Nothing();
    }

    // Step 17.b. (Loop unrolled)
    auto numericOption =
        mozilla::Some(mozilla::intl::DateTimeFormat::Numeric::Numeric);
    if (defaultOptions.contains(DateTimeField::Year)) {
      formatOptions.year = numericOption;
    }
    if (defaultOptions.contains(DateTimeField::Month)) {
      formatOptions.month =
          mozilla::Some(mozilla::intl::DateTimeFormat::Month::Numeric);
    }
    if (defaultOptions.contains(DateTimeField::Day)) {
      formatOptions.day = numericOption;
    }
    if (defaultOptions.contains(DateTimeField::Hour)) {
      formatOptions.hour = numericOption;
    }
    if (defaultOptions.contains(DateTimeField::Minute)) {
      formatOptions.minute = numericOption;
    }
    if (defaultOptions.contains(DateTimeField::Second)) {
      formatOptions.second = numericOption;
    }

    // Step 17.c.
    if (defaults == Defaults::ZonedDateTime && !formatOptions.timeZoneName) {
      formatOptions.timeZoneName =
          mozilla::Some(mozilla::intl::DateTimeFormat::TimeZoneName::Short);
    }
  }

  // Steps 18-20. (Performed in caller).

  return mozilla::Some(formatOptions);
}

/**
 * AdjustDateTimeStyleFormat ( formats, baseFormat, matcher, allowedOptions )
 *
 * https://tc39.es/proposal-temporal/#sec-adjustdatetimestyleformat
 */
static mozilla::Result<mozilla::intl::DateTimeFormat::ComponentsBag,
                       mozilla::intl::ICUError>
AdjustDateTimeStyleFormat(mozilla::intl::DateTimeFormat* baseFormat,
                          mozilla::EnumSet<DateTimeField> allowedOptions) {
  // Step 1.
  mozilla::intl::DateTimeFormat::ComponentsBag formatOptions;

  // Step 2. (Loop unrolled)
  auto result = baseFormat->ResolveComponents();
  if (result.isErr()) {
    return result.propagateErr();
  }
  auto options = result.unwrap();

  if (allowedOptions.contains(DateTimeField::Era) && options.era) {
    formatOptions.era = options.era;
  }
  if (allowedOptions.contains(DateTimeField::Weekday) && options.weekday) {
    formatOptions.weekday = options.weekday;
  }
  if (allowedOptions.contains(DateTimeField::Year) && options.year) {
    formatOptions.year = options.year;
  }
  if (allowedOptions.contains(DateTimeField::Month) && options.month) {
    formatOptions.month = options.month;
  }
  if (allowedOptions.contains(DateTimeField::Day) && options.day) {
    formatOptions.day = options.day;
  }
  if (allowedOptions.contains(DateTimeField::DayPeriod) && options.dayPeriod) {
    formatOptions.dayPeriod = options.dayPeriod;
  }
  if (allowedOptions.contains(DateTimeField::Hour) && options.hour) {
    formatOptions.hour = options.hour;
    formatOptions.hourCycle = options.hourCycle;
  }
  if (allowedOptions.contains(DateTimeField::Minute) && options.minute) {
    formatOptions.minute = options.minute;
  }
  if (allowedOptions.contains(DateTimeField::Second) && options.second) {
    formatOptions.second = options.second;
  }
  if (allowedOptions.contains(DateTimeField::FractionalSecondDigits) &&
      options.fractionalSecondDigits) {
    formatOptions.fractionalSecondDigits = options.fractionalSecondDigits;
  }

  // Steps 3-5. (Performed in caller)

  return formatOptions;
}

static const char* DateTimeValueKindToString(DateTimeValueKind kind) {
  switch (kind) {
    case DateTimeValueKind::Number:
      return "number";
    case DateTimeValueKind::TemporalDate:
      return "Temporal.PlainDate";
    case DateTimeValueKind::TemporalTime:
      return "Temporal.PlainTime";
    case DateTimeValueKind::TemporalDateTime:
      return "Temporal.PlainDateTime";
    case DateTimeValueKind::TemporalYearMonth:
      return "Temporal.PlainYearMonth";
    case DateTimeValueKind::TemporalMonthDay:
      return "Temporal.PlainMonthDay";
    case DateTimeValueKind::TemporalZonedDateTime:
      return "Temporal.ZonedDateTime";
    case DateTimeValueKind::TemporalInstant:
      return "Temporal.Instant";
  }
  MOZ_CRASH("invalid date-time value kind");
}

class TimeZoneOffsetString {
  static constexpr std::u16string_view GMT = u"GMT";

  // Time zone offset string format is "±hh:mm".
  static constexpr size_t offsetLength = 6;

  // ICU custom time zones are in the format "GMT±hh:mm".
  char16_t timeZone_[GMT.size() + offsetLength] = {};

  TimeZoneOffsetString() = default;

 public:
  TimeZoneOffsetString(const TimeZoneOffsetString& other) { *this = other; }

  TimeZoneOffsetString& operator=(const TimeZoneOffsetString& other) {
    std::copy_n(other.timeZone_, std::size(timeZone_), timeZone_);
    return *this;
  }

  operator mozilla::Span<const char16_t>() const {
    return mozilla::Span(timeZone_);
  }

  /**
   * |timeZone| is either a canonical IANA time zone identifier or a normalized
   * time zone offset string.
   */
  static mozilla::Maybe<TimeZoneOffsetString> from(
      const JSLinearString* timeZone) {
    MOZ_RELEASE_ASSERT(!timeZone->empty(), "time zone is a non-empty string");

    // If the time zone string starts with either "+" or "-", it is a normalized
    // time zone offset string, because (canonical) IANA time zone identifiers
    // can't start with "+" or "-".
    char16_t timeZoneSign = timeZone->latin1OrTwoByteChar(0);
    MOZ_ASSERT(timeZoneSign != 0x2212,
               "Minus sign is normalized to Ascii minus");
    if (timeZoneSign != '+' && timeZoneSign != '-') {
      return mozilla::Nothing();
    }

    // Release assert because we don't want CopyChars to write out-of-bounds.
    MOZ_RELEASE_ASSERT(timeZone->length() == offsetLength);

    // Self-hosted code has normalized offset strings to the format "±hh:mm".
    MOZ_ASSERT(mozilla::IsAsciiDigit(timeZone->latin1OrTwoByteChar(1)));
    MOZ_ASSERT(mozilla::IsAsciiDigit(timeZone->latin1OrTwoByteChar(2)));
    MOZ_ASSERT(timeZone->latin1OrTwoByteChar(3) == ':');
    MOZ_ASSERT(mozilla::IsAsciiDigit(timeZone->latin1OrTwoByteChar(4)));
    MOZ_ASSERT(mozilla::IsAsciiDigit(timeZone->latin1OrTwoByteChar(5)));

    // Self-hosted code has verified the offset is at most ±23:59.
#ifdef DEBUG
    auto twoDigit = [&](size_t offset) {
      auto c1 = timeZone->latin1OrTwoByteChar(offset);
      auto c2 = timeZone->latin1OrTwoByteChar(offset + 1);
      return mozilla::AsciiAlphanumericToNumber(c1) * 10 +
             mozilla::AsciiAlphanumericToNumber(c2);
    };

    int32_t hours = twoDigit(1);
    MOZ_ASSERT(0 <= hours && hours <= 23);

    int32_t minutes = twoDigit(4);
    MOZ_ASSERT(0 <= minutes && minutes <= 59);
#endif

    TimeZoneOffsetString result{};

    // Copy the string "GMT" followed by the offset string.
    size_t copied = GMT.copy(result.timeZone_, GMT.size());
    CopyChars(result.timeZone_ + copied, *timeZone);

    return mozilla::Some(result);
  }
};

/**
 * Returns a new mozilla::intl::DateTimeFormat with the locale and date-time
 * formatting options of the given DateTimeFormat.
 */
static mozilla::intl::DateTimeFormat* NewDateTimeFormat(
    JSContext* cx, Handle<DateTimeFormatObject*> dateTimeFormat,
    DateTimeValueKind kind) {
  RootedValue value(cx);

  RootedObject internals(cx, intl::GetInternalsObject(cx, dateTimeFormat));
  if (!internals) {
    return nullptr;
  }

  UniqueChars locale = DateTimeFormatLocale(cx, internals);
  if (!locale) {
    return nullptr;
  }

  if (!GetProperty(cx, internals, internals, cx->names().timeZone, &value)) {
    return nullptr;
  }

  Rooted<JSLinearString*> timeZoneString(cx,
                                         value.toString()->ensureLinear(cx));
  if (!timeZoneString) {
    return nullptr;
  }

  AutoStableStringChars timeZone(cx);
  mozilla::Span<const char16_t> timeZoneChars{};

  auto timeZoneOffset = TimeZoneOffsetString::from(timeZoneString);
  if (timeZoneOffset) {
    timeZoneChars = *timeZoneOffset;
  } else {
    if (!timeZone.initTwoByte(cx, timeZoneString)) {
      return nullptr;
    }
    timeZoneChars = timeZone.twoByteRange();
  }

  if (!GetProperty(cx, internals, internals, cx->names().pattern, &value)) {
    return nullptr;
  }
  bool hasPattern = value.isString();

  if (!GetProperty(cx, internals, internals, cx->names().timeStyle, &value)) {
    return nullptr;
  }
  bool hasStyle = value.isString();
  if (!hasStyle) {
    if (!GetProperty(cx, internals, internals, cx->names().dateStyle, &value)) {
      return nullptr;
    }
    hasStyle = value.isString();
  }

  mozilla::UniquePtr<mozilla::intl::DateTimeFormat> df = nullptr;
  if (hasPattern) {
    // This is a DateTimeFormat defined by a pattern option. This is internal
    // to Mozilla, and not part of the ECMA-402 API.
    if (!GetProperty(cx, internals, internals, cx->names().pattern, &value)) {
      return nullptr;
    }

    AutoStableStringChars pattern(cx);
    if (!pattern.initTwoByte(cx, value.toString())) {
      return nullptr;
    }

    auto dfResult = mozilla::intl::DateTimeFormat::TryCreateFromPattern(
        mozilla::MakeStringSpan(locale.get()), pattern.twoByteRange(),
        mozilla::Some(timeZoneChars));
    if (dfResult.isErr()) {
      intl::ReportInternalError(cx, dfResult.unwrapErr());
      return nullptr;
    }

    df = dfResult.unwrap();
  } else if (hasStyle) {
    // This is a DateTimeFormat defined by a time style or date style.
    mozilla::intl::DateTimeFormat::StyleBag style;
    if (!AssignDateTimeLength(cx, internals, cx->names().timeStyle,
                              &style.time)) {
      return nullptr;
    }
    if (!AssignDateTimeLength(cx, internals, cx->names().dateStyle,
                              &style.date)) {
      return nullptr;
    }
    if (!AssignHourCycleComponent(cx, internals, cx->names().hourCycle,
                                  &style.hourCycle)) {
      return nullptr;
    }

    if (!AssignHour12Component(cx, internals, &style.hour12)) {
      return nullptr;
    }

    switch (kind) {
      case DateTimeValueKind::TemporalDate:
      case DateTimeValueKind::TemporalYearMonth:
      case DateTimeValueKind::TemporalMonthDay: {
        if (!style.date) {
          JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                    JSMSG_INVALID_FORMAT_OPTIONS,
                                    DateTimeValueKindToString(kind));
          return nullptr;
        }
        break;
      }

      case DateTimeValueKind::TemporalTime: {
        if (!style.time) {
          JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                    JSMSG_INVALID_FORMAT_OPTIONS,
                                    DateTimeValueKindToString(kind));
          return nullptr;
        }
        break;
      }

      case DateTimeValueKind::Number:
      case DateTimeValueKind::TemporalDateTime:
      case DateTimeValueKind::TemporalZonedDateTime:
      case DateTimeValueKind::TemporalInstant:
        break;
    }

    SharedIntlData& sharedIntlData = cx->runtime()->sharedIntlData.ref();
    auto* dtpg = sharedIntlData.getDateTimePatternGenerator(cx, locale.get());
    if (!dtpg) {
      return nullptr;
    }

    auto dfResult = mozilla::intl::DateTimeFormat::TryCreateFromStyle(
        mozilla::MakeStringSpan(locale.get()), style, dtpg,
        mozilla::Some(timeZoneChars));
    if (dfResult.isErr()) {
      intl::ReportInternalError(cx, dfResult.unwrapErr());
      return nullptr;
    }
    df = dfResult.unwrap();

    mozilla::EnumSet<DateTimeField> allowedOptions;
    switch (kind) {
      case DateTimeValueKind::TemporalDate:
        allowedOptions = {
            DateTimeField::Weekday, DateTimeField::Era, DateTimeField::Year,
            DateTimeField::Month,   DateTimeField::Day,
        };
        break;
      case DateTimeValueKind::TemporalTime:
        allowedOptions = {
            DateTimeField::DayPeriod,
            DateTimeField::Hour,
            DateTimeField::Minute,
            DateTimeField::Second,
            DateTimeField::FractionalSecondDigits,
        };
        break;
      case DateTimeValueKind::TemporalDateTime:
        allowedOptions = {
            DateTimeField::Weekday, DateTimeField::Era,
            DateTimeField::Year,    DateTimeField::Month,
            DateTimeField::Day,     DateTimeField::DayPeriod,
            DateTimeField::Hour,    DateTimeField::Minute,
            DateTimeField::Second,  DateTimeField::FractionalSecondDigits,
        };
        break;
      case DateTimeValueKind::TemporalYearMonth:
        allowedOptions = {
            DateTimeField::Era,
            DateTimeField::Year,
            DateTimeField::Month,
        };
        break;
      case DateTimeValueKind::TemporalMonthDay:
        allowedOptions = {
            DateTimeField::Month,
            DateTimeField::Day,
        };
        break;

      case DateTimeValueKind::Number:
      case DateTimeValueKind::TemporalZonedDateTime:
      case DateTimeValueKind::TemporalInstant:
        break;
    }

    if (!allowedOptions.isEmpty()) {
      auto adjusted = AdjustDateTimeStyleFormat(df.get(), allowedOptions);
      if (adjusted.isErr()) {
        intl::ReportInternalError(cx, dfResult.unwrapErr());
        return nullptr;
      }
      auto bag = adjusted.unwrap();

      auto dfResult = mozilla::intl::DateTimeFormat::TryCreateFromComponents(
          mozilla::MakeStringSpan(locale.get()), bag, dtpg,
          mozilla::Some(timeZoneChars));
      if (dfResult.isErr()) {
        intl::ReportInternalError(cx, dfResult.unwrapErr());
        return nullptr;
      }
      df = dfResult.unwrap();
    }
  } else {
    // This is a DateTimeFormat defined by a components bag.
    mozilla::intl::DateTimeFormat::ComponentsBag bag;

    if (!AssignTextComponent(cx, internals, cx->names().era, &bag.era)) {
      return nullptr;
    }
    if (!AssignNumericComponent(cx, internals, cx->names().year, &bag.year)) {
      return nullptr;
    }
    if (!AssignMonthComponent(cx, internals, cx->names().month, &bag.month)) {
      return nullptr;
    }
    if (!AssignNumericComponent(cx, internals, cx->names().day, &bag.day)) {
      return nullptr;
    }
    if (!AssignTextComponent(cx, internals, cx->names().weekday,
                             &bag.weekday)) {
      return nullptr;
    }
    if (!AssignNumericComponent(cx, internals, cx->names().hour, &bag.hour)) {
      return nullptr;
    }
    if (!AssignNumericComponent(cx, internals, cx->names().minute,
                                &bag.minute)) {
      return nullptr;
    }
    if (!AssignNumericComponent(cx, internals, cx->names().second,
                                &bag.second)) {
      return nullptr;
    }
    if (!AssignTimeZoneNameComponent(cx, internals, cx->names().timeZoneName,
                                     &bag.timeZoneName)) {
      return nullptr;
    }
    if (!AssignHourCycleComponent(cx, internals, cx->names().hourCycle,
                                  &bag.hourCycle)) {
      return nullptr;
    }
    if (!AssignTextComponent(cx, internals, cx->names().dayPeriod,
                             &bag.dayPeriod)) {
      return nullptr;
    }
    if (!AssignHour12Component(cx, internals, &bag.hour12)) {
      return nullptr;
    }

    if (!GetProperty(cx, internals, internals,
                     cx->names().fractionalSecondDigits, &value)) {
      return nullptr;
    }
    if (value.isInt32()) {
      bag.fractionalSecondDigits = mozilla::Some(value.toInt32());
    } else {
      MOZ_ASSERT(value.isUndefined());
    }

    DateTimeFormatArgs dateTimeFormatArgs;
    if (!GetDateTimeFormatArgs(cx, internals, kind, &dateTimeFormatArgs)) {
      return nullptr;
    }
    auto [required, defaults, inherit] = dateTimeFormatArgs;

    auto resolvedBag = GetDateTimeFormat(bag, required, defaults, inherit);
    if (!resolvedBag) {
      JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                JSMSG_INVALID_FORMAT_OPTIONS,
                                DateTimeValueKindToString(kind));
      return nullptr;
    }
    bag = *resolvedBag;

    SharedIntlData& sharedIntlData = cx->runtime()->sharedIntlData.ref();
    auto* dtpg = sharedIntlData.getDateTimePatternGenerator(cx, locale.get());
    if (!dtpg) {
      return nullptr;
    }

    auto dfResult = mozilla::intl::DateTimeFormat::TryCreateFromComponents(
        mozilla::MakeStringSpan(locale.get()), bag, dtpg,
        mozilla::Some(timeZoneChars));
    if (dfResult.isErr()) {
      intl::ReportInternalError(cx, dfResult.unwrapErr());
      return nullptr;
    }
    df = dfResult.unwrap();
  }

  return df.release();
}

void js::DateTimeFormatObject::maybeClearCache(DateTimeValueKind kind) {
  if (getDateTimeValueKind() == kind) {
    return;
  }
  setDateTimeValueKind(kind);

  if (auto* df = getDateFormat()) {
    intl::RemoveICUCellMemory(
        this, DateTimeFormatObject::UDateFormatEstimatedMemoryUse);
    delete df;

    setDateFormat(nullptr);
  }

  if (auto* dif = getDateIntervalFormat()) {
    intl::RemoveICUCellMemory(
        this, DateTimeFormatObject::UDateIntervalFormatEstimatedMemoryUse);
    delete dif;

    setDateIntervalFormat(nullptr);
  }
}

static mozilla::intl::DateTimeFormat* GetOrCreateDateTimeFormat(
    JSContext* cx, Handle<DateTimeFormatObject*> dateTimeFormat,
    DateTimeValueKind kind) {
  // Clear previously created formatters if their type doesn't match.
  dateTimeFormat->maybeClearCache(kind);

  // Obtain a cached mozilla::intl::DateTimeFormat object.
  mozilla::intl::DateTimeFormat* df = dateTimeFormat->getDateFormat();
  if (df) {
    return df;
  }

  df = NewDateTimeFormat(cx, dateTimeFormat, kind);
  if (!df) {
    return nullptr;
  }
  dateTimeFormat->setDateFormat(df);

  intl::AddICUCellMemory(dateTimeFormat,
                         DateTimeFormatObject::UDateFormatEstimatedMemoryUse);
  return df;
}

template <typename T>
static bool SetResolvedProperty(JSContext* cx, HandleObject resolved,
                                Handle<PropertyName*> name,
                                mozilla::Maybe<T> intlProp) {
  if (!intlProp) {
    return true;
  }
  JSString* str = NewStringCopyZ<CanGC>(
      cx, mozilla::intl::DateTimeFormat::ToString(*intlProp));
  if (!str) {
    return false;
  }
  RootedValue value(cx, StringValue(str));
  return DefineDataProperty(cx, resolved, name, value);
}

bool js::intl_resolveDateTimeFormatComponents(JSContext* cx, unsigned argc,
                                              Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(args[1].isObject());
  MOZ_ASSERT(args[2].isBoolean());

  Rooted<DateTimeFormatObject*> dateTimeFormat(cx);
  dateTimeFormat = &args[0].toObject().as<DateTimeFormatObject>();

  RootedObject resolved(cx, &args[1].toObject());

  bool includeDateTimeFields = args[2].toBoolean();

  mozilla::intl::DateTimeFormat* df =
      GetOrCreateDateTimeFormat(cx, dateTimeFormat, DateTimeValueKind::Number);
  if (!df) {
    return false;
  }

  auto result = df->ResolveComponents();
  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return false;
  }

  mozilla::intl::DateTimeFormat::ComponentsBag components = result.unwrap();

  // Map the resolved mozilla::intl::DateTimeFormat::ComponentsBag to the
  // options object as returned by DateTimeFormat.prototype.resolvedOptions.
  //
  // Resolved options must match the ordering as defined in:
  // https://tc39.es/ecma402/#sec-intl.datetimeformat.prototype.resolvedoptions

  if (!SetResolvedProperty(cx, resolved, cx->names().hourCycle,
                           components.hourCycle)) {
    return false;
  }

  if (components.hour12) {
    RootedValue value(cx, BooleanValue(*components.hour12));
    if (!DefineDataProperty(cx, resolved, cx->names().hour12, value)) {
      return false;
    }
  }

  if (!includeDateTimeFields) {
    args.rval().setUndefined();
    // Do not include date time fields.
    return true;
  }

  if (!SetResolvedProperty(cx, resolved, cx->names().weekday,
                           components.weekday)) {
    return false;
  }
  if (!SetResolvedProperty(cx, resolved, cx->names().era, components.era)) {
    return false;
  }
  if (!SetResolvedProperty(cx, resolved, cx->names().year, components.year)) {
    return false;
  }
  if (!SetResolvedProperty(cx, resolved, cx->names().month, components.month)) {
    return false;
  }
  if (!SetResolvedProperty(cx, resolved, cx->names().day, components.day)) {
    return false;
  }
  if (!SetResolvedProperty(cx, resolved, cx->names().dayPeriod,
                           components.dayPeriod)) {
    return false;
  }
  if (!SetResolvedProperty(cx, resolved, cx->names().hour, components.hour)) {
    return false;
  }
  if (!SetResolvedProperty(cx, resolved, cx->names().minute,
                           components.minute)) {
    return false;
  }
  if (!SetResolvedProperty(cx, resolved, cx->names().second,
                           components.second)) {
    return false;
  }
  if (!SetResolvedProperty(cx, resolved, cx->names().timeZoneName,
                           components.timeZoneName)) {
    return false;
  }

  if (components.fractionalSecondDigits) {
    RootedValue value(cx, Int32Value(*components.fractionalSecondDigits));
    if (!DefineDataProperty(cx, resolved, cx->names().fractionalSecondDigits,
                            value)) {
      return false;
    }
  }

  args.rval().setUndefined();
  return true;
}

/**
 * ToDateTimeFormattable ( value )
 *
 * https://tc39.es/proposal-temporal/#sec-todatetimeformattable
 */
static auto ToDateTimeFormattable(const Value& value) {
  // Step 1. (Inlined IsTemporalObject)
  if (value.isObject()) {
    auto* obj = CheckedUnwrapStatic(&value.toObject());
    if (obj) {
      if (obj->is<PlainDateObject>()) {
        return DateTimeValueKind::TemporalDate;
      }
      if (obj->is<PlainDateTimeObject>()) {
        return DateTimeValueKind::TemporalDateTime;
      }
      if (obj->is<PlainTimeObject>()) {
        return DateTimeValueKind::TemporalTime;
      }
      if (obj->is<PlainYearMonthObject>()) {
        return DateTimeValueKind::TemporalYearMonth;
      }
      if (obj->is<PlainMonthDayObject>()) {
        return DateTimeValueKind::TemporalMonthDay;
      }
      if (obj->is<ZonedDateTimeObject>()) {
        return DateTimeValueKind::TemporalZonedDateTime;
      }
      if (obj->is<InstantObject>()) {
        return DateTimeValueKind::TemporalInstant;
      }
      return DateTimeValueKind::Number;
    }
  }

  // Step 2. (ToNumber performed in caller)
  return DateTimeValueKind::Number;
}

static bool ResolveCalendarAndTimeZone(
    JSContext* cx, Handle<DateTimeFormatObject*> dateTimeFormat) {
  Rooted<JSObject*> internals(cx, intl::GetInternalsObject(cx, dateTimeFormat));
  if (!internals) {
    return false;
  }

  Rooted<Value> calendarValue(cx);
  if (!GetProperty(cx, internals, internals, cx->names().calendar,
                   &calendarValue)) {
    return false;
  }
  Rooted<JSString*> calendarString(cx, calendarValue.toString());

  Rooted<CalendarValue> calendar(cx);
  if (!CanonicalizeCalendar(cx, calendarString, &calendar)) {
    return false;
  }

  Rooted<Value> timeZoneValue(cx);
  if (!GetProperty(cx, internals, internals, cx->names().timeZone,
                   &timeZoneValue)) {
    return false;
  }
  Rooted<JSString*> timeZoneString(cx, timeZoneValue.toString());

  Rooted<ParsedTimeZone> parsedTimeZone(cx);
  Rooted<TimeZoneValue> timeZone(cx);
  if (!ParseTemporalTimeZoneString(cx, timeZoneString, &parsedTimeZone) ||
      !ToTemporalTimeZone(cx, parsedTimeZone, &timeZone)) {
    return false;
  }

  dateTimeFormat->setCalendar(calendar);
  dateTimeFormat->setTimeZone(timeZone);
  return true;
}

/**
 * HandleDateTimeTemporalDate ( dateTimeFormat, temporalDate )
 *
 * https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporaldate
 */
static bool HandleDateTimeTemporalDate(
    JSContext* cx, Handle<DateTimeFormatObject*> dateTimeFormat,
    Handle<PlainDateObject*> unwrappedTemporalDate, ClippedTime* result) {
  auto isoDate = unwrappedTemporalDate->date();
  auto calendarId = unwrappedTemporalDate->calendar().identifier();

  Rooted<CalendarValue> calendar(cx, dateTimeFormat->getCalendar());
  Rooted<TimeZoneValue> timeZone(cx, dateTimeFormat->getTimeZone());
  if (!calendar || !timeZone) {
    if (!ResolveCalendarAndTimeZone(cx, dateTimeFormat)) {
      return false;
    }
    calendar.set(dateTimeFormat->getCalendar());
    timeZone.set(dateTimeFormat->getTimeZone());
  }
  MOZ_ASSERT(calendar && timeZone);

  // Step 1.
  if (calendarId != CalendarId::ISO8601 &&
      calendarId != calendar.identifier()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INCOMPATIBLE,
                              CalendarIdentifier(calendarId).data(),
                              CalendarIdentifier(calendar).data());
    return false;
  }

  // Step 2.
  auto isoDateTime = ISODateTime{isoDate, {12, 0, 0}};

  // Step 3.
  EpochNanoseconds epochNs;
  if (!GetEpochNanosecondsFor(cx, timeZone, isoDateTime,
                              TemporalDisambiguation::Compatible, &epochNs)) {
    return false;
  }

  // Steps 4-5. (Performed in NewDateTimeFormat)

  // Step 6.
  int64_t milliseconds = epochNs.floorToMilliseconds();
  *result = JS::TimeClip(double(milliseconds));
  return true;
}

/**
 * HandleDateTimeTemporalYearMonth ( dateTimeFormat, temporalYearMonth )
 *
 * https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporalyearmonth
 */
static bool HandleDateTimeTemporalYearMonth(
    JSContext* cx, Handle<DateTimeFormatObject*> dateTimeFormat,
    Handle<PlainYearMonthObject*> unwrappedTemporalYearMonth,
    ClippedTime* result) {
  auto isoDate = unwrappedTemporalYearMonth->date();
  auto calendarId = unwrappedTemporalYearMonth->calendar().identifier();

  Rooted<CalendarValue> calendar(cx, dateTimeFormat->getCalendar());
  Rooted<TimeZoneValue> timeZone(cx, dateTimeFormat->getTimeZone());
  if (!calendar || !timeZone) {
    if (!ResolveCalendarAndTimeZone(cx, dateTimeFormat)) {
      return false;
    }
    calendar.set(dateTimeFormat->getCalendar());
    timeZone.set(dateTimeFormat->getTimeZone());
  }
  MOZ_ASSERT(calendar && timeZone);

  // Step 1.
  if (calendarId != calendar.identifier()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INCOMPATIBLE,
                              CalendarIdentifier(calendarId).data(),
                              CalendarIdentifier(calendar).data());
    return false;
  }

  // Step 2.
  auto isoDateTime = ISODateTime{isoDate, {12, 0, 0}};

  // Step 3.
  EpochNanoseconds epochNs;
  if (!GetEpochNanosecondsFor(cx, timeZone, isoDateTime,
                              TemporalDisambiguation::Compatible, &epochNs)) {
    return false;
  }

  // Steps 4-5. (Performed in NewDateTimeFormat)

  // Step 6.
  int64_t milliseconds = epochNs.floorToMilliseconds();
  *result = JS::TimeClip(double(milliseconds));
  return true;
}

/**
 * HandleDateTimeTemporalMonthDay ( dateTimeFormat, temporalMonthDay )
 *
 * https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporalmonthday
 */
static bool HandleDateTimeTemporalMonthDay(
    JSContext* cx, Handle<DateTimeFormatObject*> dateTimeFormat,
    Handle<PlainMonthDayObject*> unwrappedTemporalMonthDay,
    ClippedTime* result) {
  auto isoDate = unwrappedTemporalMonthDay->date();
  auto calendarId = unwrappedTemporalMonthDay->calendar().identifier();

  Rooted<CalendarValue> calendar(cx, dateTimeFormat->getCalendar());
  Rooted<TimeZoneValue> timeZone(cx, dateTimeFormat->getTimeZone());
  if (!calendar || !timeZone) {
    if (!ResolveCalendarAndTimeZone(cx, dateTimeFormat)) {
      return false;
    }
    calendar.set(dateTimeFormat->getCalendar());
    timeZone.set(dateTimeFormat->getTimeZone());
  }
  MOZ_ASSERT(calendar && timeZone);

  // Step 1.
  if (calendarId != calendar.identifier()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INCOMPATIBLE,
                              CalendarIdentifier(calendarId).data(),
                              CalendarIdentifier(calendar).data());
    return false;
  }

  // Step 2.
  auto isoDateTime = ISODateTime{isoDate, {12, 0, 0}};

  // Step 3.
  EpochNanoseconds epochNs;
  if (!GetEpochNanosecondsFor(cx, timeZone, isoDateTime,
                              TemporalDisambiguation::Compatible, &epochNs)) {
    return false;
  }

  // Steps 4-5. (Performed in NewDateTimeFormat)

  // Step 6.
  int64_t milliseconds = epochNs.floorToMilliseconds();
  *result = JS::TimeClip(double(milliseconds));
  return true;
}

/**
 * HandleDateTimeTemporalTime ( dateTimeFormat, temporalTime )
 *
 * https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporaltime
 */
static bool HandleDateTimeTemporalTime(
    JSContext* cx, Handle<DateTimeFormatObject*> dateTimeFormat,
    Handle<PlainTimeObject*> unwrappedTemporalTime, ClippedTime* result) {
  auto time = unwrappedTemporalTime->time();

  Rooted<TimeZoneValue> timeZone(cx, dateTimeFormat->getTimeZone());
  if (!timeZone) {
    if (!ResolveCalendarAndTimeZone(cx, dateTimeFormat)) {
      return false;
    }
    timeZone.set(dateTimeFormat->getTimeZone());
  }
  MOZ_ASSERT(timeZone);

  // Steps 1-2.
  auto isoDateTime = ISODateTime{{1970, 1, 1}, time};

  // Step 3.
  EpochNanoseconds epochNs;
  if (!GetEpochNanosecondsFor(cx, timeZone, isoDateTime,
                              TemporalDisambiguation::Compatible, &epochNs)) {
    return false;
  }

  // Steps 4-5. (Performed in NewDateTimeFormat)

  // Step 6.
  int64_t milliseconds = epochNs.floorToMilliseconds();
  *result = JS::TimeClip(double(milliseconds));
  return true;
}

/**
 * HandleDateTimeTemporalDateTime ( dateTimeFormat, dateTime )
 *
 * https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporaldatetime
 */
static bool HandleDateTimeTemporalDateTime(
    JSContext* cx, Handle<DateTimeFormatObject*> dateTimeFormat,
    Handle<PlainDateTimeObject*> unwrappedDateTime, ClippedTime* result) {
  auto isoDateTime = unwrappedDateTime->dateTime();
  auto calendarId = unwrappedDateTime->calendar().identifier();

  Rooted<CalendarValue> calendar(cx, dateTimeFormat->getCalendar());
  Rooted<TimeZoneValue> timeZone(cx, dateTimeFormat->getTimeZone());
  if (!calendar || !timeZone) {
    if (!ResolveCalendarAndTimeZone(cx, dateTimeFormat)) {
      return false;
    }
    calendar.set(dateTimeFormat->getCalendar());
    timeZone.set(dateTimeFormat->getTimeZone());
  }
  MOZ_ASSERT(calendar && timeZone);

  // Step 1.
  if (calendarId != CalendarId::ISO8601 &&
      calendarId != calendar.identifier()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INCOMPATIBLE,
                              CalendarIdentifier(calendarId).data(),
                              CalendarIdentifier(calendar).data());
    return false;
  }

  // Step 2.
  EpochNanoseconds epochNs;
  if (!GetEpochNanosecondsFor(cx, timeZone, isoDateTime,
                              TemporalDisambiguation::Compatible, &epochNs)) {
    return false;
  }

  // Step 3. (Performed in NewDateTimeFormat)

  // Step 4.
  int64_t milliseconds = epochNs.floorToMilliseconds();
  *result = JS::TimeClip(double(milliseconds));
  return true;
}

/**
 * HandleDateTimeTemporalInstant ( dateTimeFormat, instant )
 *
 * https://tc39.es/proposal-temporal/#sec-temporal-handledatetimetemporalinstant
 */
static bool HandleDateTimeTemporalInstant(InstantObject* unwrappedInstant,
                                          ClippedTime* result) {
  // Step 1. (Performed in NewDateTimeFormat)

  // Step 2.
  auto epochNs = unwrappedInstant->epochNanoseconds();
  int64_t milliseconds = epochNs.floorToMilliseconds();
  *result = JS::TimeClip(double(milliseconds));
  return true;
}

/**
 * Temporal.ZonedDateTime.prototype.toLocaleString ( [ locales [ , options ] ] )
 */
static bool HandleDateTimeTemporalZonedDateTime(
    JSContext* cx, Handle<DateTimeFormatObject*> dateTimeFormat,
    Handle<ZonedDateTimeObject*> unwrappedZonedDateTime, ClippedTime* result) {
  auto epochNs = unwrappedZonedDateTime->epochNanoseconds();
  auto calendarId = unwrappedZonedDateTime->calendar().identifier();

  Rooted<CalendarValue> calendar(cx, dateTimeFormat->getCalendar());
  Rooted<TimeZoneValue> timeZone(cx, dateTimeFormat->getTimeZone());
  if (!calendar || !timeZone) {
    if (!ResolveCalendarAndTimeZone(cx, dateTimeFormat)) {
      return false;
    }
    calendar.set(dateTimeFormat->getCalendar());
    timeZone.set(dateTimeFormat->getTimeZone());
  }
  MOZ_ASSERT(calendar && timeZone);

  // Step 4.
  if (calendarId != CalendarId::ISO8601 &&
      calendarId != calendar.identifier()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_TEMPORAL_CALENDAR_INCOMPATIBLE,
                              CalendarIdentifier(calendarId).data(),
                              CalendarIdentifier(calendar).data());
    return false;
  }

  // Step 5.
  int64_t milliseconds = epochNs.floorToMilliseconds();
  *result = JS::TimeClip(double(milliseconds));
  return true;
}

/**
 * HandleDateTimeOthers ( dateTimeFormat, x )
 *
 * https://tc39.es/proposal-temporal/#sec-temporal-handledatetimeothers
 */
static bool HandleDateTimeOthers(JSContext* cx, const char* method, double x,
                                 ClippedTime* result) {
  // Step 1.
  auto clipped = JS::TimeClip(x);

  // Step 2.
  if (!clipped.isValid()) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_DATE_NOT_FINITE, "DateTimeFormat", method);
    return false;
  }

  // Step 4. (Performed in NewDateTimeFormat)

  // Steps 3 and 5.
  *result = clipped;
  return true;
}

/**
 * HandleDateTimeValue ( dateTimeFormat, x )
 *
 * https://tc39.es/proposal-temporal/#sec-temporal-handledatetimevalue
 */
static bool HandleDateTimeValue(JSContext* cx, const char* method,
                                Handle<DateTimeFormatObject*> dateTimeFormat,
                                Handle<Value> x, ClippedTime* result) {
  MOZ_ASSERT(x.isObject() || x.isNumber());

  // Step 1.
  if (x.isObject()) {
    Rooted<JSObject*> unwrapped(cx, CheckedUnwrapStatic(&x.toObject()));
    if (!unwrapped) {
      ReportAccessDenied(cx);
      return false;
    }

    // Step 1.a.
    if (unwrapped->is<PlainDateObject>()) {
      return HandleDateTimeTemporalDate(
          cx, dateTimeFormat, unwrapped.as<PlainDateObject>(), result);
    }

    // Step 1.b.
    if (unwrapped->is<PlainYearMonthObject>()) {
      return HandleDateTimeTemporalYearMonth(
          cx, dateTimeFormat, unwrapped.as<PlainYearMonthObject>(), result);
    }

    // Step 1.c.
    if (unwrapped->is<PlainMonthDayObject>()) {
      return HandleDateTimeTemporalMonthDay(
          cx, dateTimeFormat, unwrapped.as<PlainMonthDayObject>(), result);
    }

    // Step 1.d.
    if (unwrapped->is<PlainTimeObject>()) {
      return HandleDateTimeTemporalTime(
          cx, dateTimeFormat, unwrapped.as<PlainTimeObject>(), result);
    }

    // Step 1.e.
    if (unwrapped->is<PlainDateTimeObject>()) {
      return HandleDateTimeTemporalDateTime(
          cx, dateTimeFormat, unwrapped.as<PlainDateTimeObject>(), result);
    }

    // Step 1.f.
    if (unwrapped->is<InstantObject>()) {
      return HandleDateTimeTemporalInstant(&unwrapped->as<InstantObject>(),
                                           result);
    }

    // Step 1.g.
    MOZ_ASSERT(unwrapped->is<ZonedDateTimeObject>());

    // Step 1.h.
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_UNEXPECTED_TYPE, "object",
                              unwrapped->getClass()->name);
    return false;
  }

  // Step 2.
  return HandleDateTimeOthers(cx, method, x.toNumber(), result);
}

static bool intl_FormatDateTime(JSContext* cx,
                                const mozilla::intl::DateTimeFormat* df,
                                ClippedTime x, MutableHandleValue result) {
  MOZ_ASSERT(x.isValid());

  FormatBuffer<char16_t, INITIAL_CHAR_BUFFER_SIZE> buffer(cx);
  auto dfResult = df->TryFormat(x.toDouble(), buffer);
  if (dfResult.isErr()) {
    intl::ReportInternalError(cx, dfResult.unwrapErr());
    return false;
  }

  JSString* str = buffer.toString(cx);
  if (!str) {
    return false;
  }

  result.setString(str);
  return true;
}

using FieldType = js::ImmutableTenuredPtr<PropertyName*> JSAtomState::*;

static FieldType GetFieldTypeForPartType(mozilla::intl::DateTimePartType type) {
  switch (type) {
    case mozilla::intl::DateTimePartType::Literal:
      return &JSAtomState::literal;
    case mozilla::intl::DateTimePartType::Era:
      return &JSAtomState::era;
    case mozilla::intl::DateTimePartType::Year:
      return &JSAtomState::year;
    case mozilla::intl::DateTimePartType::YearName:
      return &JSAtomState::yearName;
    case mozilla::intl::DateTimePartType::RelatedYear:
      return &JSAtomState::relatedYear;
    case mozilla::intl::DateTimePartType::Month:
      return &JSAtomState::month;
    case mozilla::intl::DateTimePartType::Day:
      return &JSAtomState::day;
    case mozilla::intl::DateTimePartType::Hour:
      return &JSAtomState::hour;
    case mozilla::intl::DateTimePartType::Minute:
      return &JSAtomState::minute;
    case mozilla::intl::DateTimePartType::Second:
      return &JSAtomState::second;
    case mozilla::intl::DateTimePartType::Weekday:
      return &JSAtomState::weekday;
    case mozilla::intl::DateTimePartType::DayPeriod:
      return &JSAtomState::dayPeriod;
    case mozilla::intl::DateTimePartType::TimeZoneName:
      return &JSAtomState::timeZoneName;
    case mozilla::intl::DateTimePartType::FractionalSecondDigits:
      return &JSAtomState::fractionalSecond;
    case mozilla::intl::DateTimePartType::Unknown:
      return &JSAtomState::unknown;
  }

  MOZ_CRASH(
      "unenumerated, undocumented format field returned "
      "by iterator");
}

static FieldType GetFieldTypeForPartSource(
    mozilla::intl::DateTimePartSource source) {
  switch (source) {
    case mozilla::intl::DateTimePartSource::Shared:
      return &JSAtomState::shared;
    case mozilla::intl::DateTimePartSource::StartRange:
      return &JSAtomState::startRange;
    case mozilla::intl::DateTimePartSource::EndRange:
      return &JSAtomState::endRange;
  }

  MOZ_CRASH(
      "unenumerated, undocumented format field returned "
      "by iterator");
}

// A helper function to create an ArrayObject from DateTimePart objects.
// When hasNoSource is true, we don't need to create the ||Source|| property for
// the DateTimePart object.
static bool CreateDateTimePartArray(
    JSContext* cx, mozilla::Span<const char16_t> formattedSpan,
    bool hasNoSource, const mozilla::intl::DateTimePartVector& parts,
    MutableHandleValue result) {
  RootedString overallResult(cx, NewStringCopy<CanGC>(cx, formattedSpan));
  if (!overallResult) {
    return false;
  }

  Rooted<ArrayObject*> partsArray(
      cx, NewDenseFullyAllocatedArray(cx, parts.length()));
  if (!partsArray) {
    return false;
  }
  partsArray->ensureDenseInitializedLength(0, parts.length());

  if (overallResult->length() == 0) {
    // An empty string contains no parts, so avoid extra work below.
    result.setObject(*partsArray);
    return true;
  }

  RootedObject singlePart(cx);
  RootedValue val(cx);

  size_t index = 0;
  size_t beginIndex = 0;
  for (const mozilla::intl::DateTimePart& part : parts) {
    singlePart = NewPlainObject(cx);
    if (!singlePart) {
      return false;
    }

    FieldType type = GetFieldTypeForPartType(part.mType);
    val = StringValue(cx->names().*type);
    if (!DefineDataProperty(cx, singlePart, cx->names().type, val)) {
      return false;
    }

    MOZ_ASSERT(part.mEndIndex > beginIndex);
    JSLinearString* partStr = NewDependentString(cx, overallResult, beginIndex,
                                                 part.mEndIndex - beginIndex);
    if (!partStr) {
      return false;
    }
    val = StringValue(partStr);
    if (!DefineDataProperty(cx, singlePart, cx->names().value, val)) {
      return false;
    }

    if (!hasNoSource) {
      FieldType source = GetFieldTypeForPartSource(part.mSource);
      val = StringValue(cx->names().*source);
      if (!DefineDataProperty(cx, singlePart, cx->names().source, val)) {
        return false;
      }
    }

    beginIndex = part.mEndIndex;
    partsArray->initDenseElement(index++, ObjectValue(*singlePart));
  }

  MOZ_ASSERT(index == parts.length());
  MOZ_ASSERT(beginIndex == formattedSpan.size());
  result.setObject(*partsArray);
  return true;
}

static bool intl_FormatToPartsDateTime(JSContext* cx,
                                       const mozilla::intl::DateTimeFormat* df,
                                       ClippedTime x, bool hasNoSource,
                                       MutableHandleValue result) {
  MOZ_ASSERT(x.isValid());

  FormatBuffer<char16_t, intl::INITIAL_CHAR_BUFFER_SIZE> buffer(cx);
  mozilla::intl::DateTimePartVector parts;
  auto r = df->TryFormatToParts(x.toDouble(), buffer, parts);
  if (r.isErr()) {
    intl::ReportInternalError(cx, r.unwrapErr());
    return false;
  }

  return CreateDateTimePartArray(cx, buffer, hasNoSource, parts, result);
}

bool js::intl_FormatDateTime(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 3);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(args[2].isBoolean());

  Rooted<DateTimeFormatObject*> dateTimeFormat(cx);
  dateTimeFormat = &args[0].toObject().as<DateTimeFormatObject>();

  bool formatToParts = args[2].toBoolean();
  const char* method = formatToParts ? "formatToParts" : "format";

  auto kind = ToDateTimeFormattable(args[1]);

  JS::ClippedTime x;
  if (!args[1].isUndefined()) {
    Rooted<Value> value(cx, args[1]);
    if (kind == DateTimeValueKind::Number) {
      if (!ToNumber(cx, &value)) {
        return false;
      }
    }
    MOZ_ASSERT(value.isNumber() || value.isObject());

    if (!HandleDateTimeValue(cx, method, dateTimeFormat, value, &x)) {
      return false;
    }
  } else {
    x = DateNow(cx);
  }
  MOZ_ASSERT(x.isValid());

  mozilla::intl::DateTimeFormat* df =
      GetOrCreateDateTimeFormat(cx, dateTimeFormat, kind);
  if (!df) {
    return false;
  }

  // Use the DateTimeFormat to actually format the time stamp.
  return formatToParts ? intl_FormatToPartsDateTime(
                             cx, df, x, /* hasNoSource */ true, args.rval())
                       : intl_FormatDateTime(cx, df, x, args.rval());
}

/**
 * Returns a new DateIntervalFormat with the locale and date-time formatting
 * options of the given DateTimeFormat.
 */
static mozilla::intl::DateIntervalFormat* NewDateIntervalFormat(
    JSContext* cx, Handle<DateTimeFormatObject*> dateTimeFormat,
    mozilla::intl::DateTimeFormat& mozDtf) {
  RootedValue value(cx);
  RootedObject internals(cx, intl::GetInternalsObject(cx, dateTimeFormat));
  if (!internals) {
    return nullptr;
  }

  FormatBuffer<char16_t, intl::INITIAL_CHAR_BUFFER_SIZE> pattern(cx);
  auto result = mozDtf.GetPattern(pattern);
  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return nullptr;
  }

  // Determine the hour cycle used in the resolved pattern.
  mozilla::Maybe<mozilla::intl::DateTimeFormat::HourCycle> hcPattern =
      mozilla::intl::DateTimeFormat::HourCycleFromPattern(pattern);

  UniqueChars locale = DateTimeFormatLocale(cx, internals, hcPattern);
  if (!locale) {
    return nullptr;
  }

  if (!GetProperty(cx, internals, internals, cx->names().timeZone, &value)) {
    return nullptr;
  }

  Rooted<JSLinearString*> timeZoneString(cx,
                                         value.toString()->ensureLinear(cx));
  if (!timeZoneString) {
    return nullptr;
  }

  AutoStableStringChars timeZone(cx);
  mozilla::Span<const char16_t> timeZoneChars{};

  auto timeZoneOffset = TimeZoneOffsetString::from(timeZoneString);
  if (timeZoneOffset) {
    timeZoneChars = *timeZoneOffset;
  } else {
    if (!timeZone.initTwoByte(cx, timeZoneString)) {
      return nullptr;
    }
    timeZoneChars = timeZone.twoByteRange();
  }

  FormatBuffer<char16_t, intl::INITIAL_CHAR_BUFFER_SIZE> skeleton(cx);
  auto skelResult = mozDtf.GetOriginalSkeleton(skeleton);
  if (skelResult.isErr()) {
    intl::ReportInternalError(cx, skelResult.unwrapErr());
    return nullptr;
  }

  auto dif = mozilla::intl::DateIntervalFormat::TryCreate(
      mozilla::MakeStringSpan(locale.get()), skeleton, timeZoneChars);

  if (dif.isErr()) {
    js::intl::ReportInternalError(cx, dif.unwrapErr());
    return nullptr;
  }

  return dif.unwrap().release();
}

static mozilla::intl::DateIntervalFormat* GetOrCreateDateIntervalFormat(
    JSContext* cx, Handle<DateTimeFormatObject*> dateTimeFormat,
    mozilla::intl::DateTimeFormat& mozDtf, DateTimeValueKind kind) {
  dateTimeFormat->maybeClearCache(kind);

  // Obtain a cached DateIntervalFormat object.
  mozilla::intl::DateIntervalFormat* dif =
      dateTimeFormat->getDateIntervalFormat();
  if (dif) {
    return dif;
  }

  dif = NewDateIntervalFormat(cx, dateTimeFormat, mozDtf);
  if (!dif) {
    return nullptr;
  }
  dateTimeFormat->setDateIntervalFormat(dif);

  intl::AddICUCellMemory(
      dateTimeFormat,
      DateTimeFormatObject::UDateIntervalFormatEstimatedMemoryUse);
  return dif;
}

/**
 * PartitionDateTimeRangePattern ( dateTimeFormat, x, y )
 */
static bool PartitionDateTimeRangePattern(
    JSContext* cx, const mozilla::intl::DateTimeFormat* df,
    const mozilla::intl::DateIntervalFormat* dif,
    mozilla::intl::AutoFormattedDateInterval& formatted, ClippedTime x,
    ClippedTime y, bool* equal) {
  MOZ_ASSERT(x.isValid());
  MOZ_ASSERT(y.isValid());

  auto result =
      dif->TryFormatDateTime(x.toDouble(), y.toDouble(), df, formatted, equal);
  if (result.isErr()) {
    intl::ReportInternalError(cx, result.unwrapErr());
    return false;
  }
  return true;
}

/**
 * FormatDateTimeRange( dateTimeFormat, x, y )
 */
static bool FormatDateTimeRange(JSContext* cx,
                                const mozilla::intl::DateTimeFormat* df,
                                const mozilla::intl::DateIntervalFormat* dif,
                                ClippedTime x, ClippedTime y,
                                MutableHandleValue result) {
  mozilla::intl::AutoFormattedDateInterval formatted;
  if (!formatted.IsValid()) {
    intl::ReportInternalError(cx, formatted.GetError());
    return false;
  }

  bool equal;
  if (!PartitionDateTimeRangePattern(cx, df, dif, formatted, x, y, &equal)) {
    return false;
  }

  // PartitionDateTimeRangePattern, step 12.
  if (equal) {
    return intl_FormatDateTime(cx, df, x, result);
  }

  auto spanResult = formatted.ToSpan();
  if (spanResult.isErr()) {
    intl::ReportInternalError(cx, spanResult.unwrapErr());
    return false;
  }
  JSString* resultStr = NewStringCopy<CanGC>(cx, spanResult.unwrap());
  if (!resultStr) {
    return false;
  }

  result.setString(resultStr);
  return true;
}

/**
 * FormatDateTimeRangeToParts ( dateTimeFormat, x, y )
 */
static bool FormatDateTimeRangeToParts(
    JSContext* cx, const mozilla::intl::DateTimeFormat* df,
    const mozilla::intl::DateIntervalFormat* dif, ClippedTime x, ClippedTime y,
    MutableHandleValue result) {
  mozilla::intl::AutoFormattedDateInterval formatted;
  if (!formatted.IsValid()) {
    intl::ReportInternalError(cx, formatted.GetError());
    return false;
  }

  bool equal;
  if (!PartitionDateTimeRangePattern(cx, df, dif, formatted, x, y, &equal)) {
    return false;
  }

  // PartitionDateTimeRangePattern, step 12.
  if (equal) {
    return intl_FormatToPartsDateTime(cx, df, x, /* hasNoSource */ false,
                                      result);
  }

  mozilla::intl::DateTimePartVector parts;
  auto r = dif->TryFormattedToParts(formatted, parts);
  if (r.isErr()) {
    intl::ReportInternalError(cx, r.unwrapErr());
    return false;
  }

  auto spanResult = formatted.ToSpan();
  if (spanResult.isErr()) {
    intl::ReportInternalError(cx, spanResult.unwrapErr());
    return false;
  }
  return CreateDateTimePartArray(cx, spanResult.unwrap(),
                                 /* hasNoSource */ false, parts, result);
}

bool js::intl_FormatDateTimeRange(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);
  MOZ_ASSERT(args.length() == 4);
  MOZ_ASSERT(args[0].isObject());
  MOZ_ASSERT(!args[1].isUndefined());
  MOZ_ASSERT(!args[2].isUndefined());
  MOZ_ASSERT(args[3].isBoolean());

  Rooted<DateTimeFormatObject*> dateTimeFormat(cx);
  dateTimeFormat = &args[0].toObject().as<DateTimeFormatObject>();

  bool formatToParts = args[3].toBoolean();
  const char* method = formatToParts ? "formatRangeToParts" : "formatRange";

  Rooted<Value> start(cx, args[1]);
  auto startKind = ToDateTimeFormattable(start);
  if (startKind == DateTimeValueKind::Number) {
    if (!ToNumber(cx, &start)) {
      return false;
    }
  }
  MOZ_ASSERT(start.isNumber() || start.isObject());

  Rooted<Value> end(cx, args[2]);
  auto endKind = ToDateTimeFormattable(end);
  if (endKind == DateTimeValueKind::Number) {
    if (!ToNumber(cx, &end)) {
      return false;
    }
  }
  MOZ_ASSERT(end.isNumber() || end.isObject());

  if (startKind != endKind) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_NOT_EXPECTED_TYPE, method,
                              DateTimeValueKindToString(startKind),
                              DateTimeValueKindToString(endKind));
    return false;
  }

  // PartitionDateTimeRangePattern, steps 1-2.
  JS::ClippedTime x;
  if (!HandleDateTimeValue(cx, method, dateTimeFormat, start, &x)) {
    return false;
  }
  MOZ_ASSERT(x.isValid());

  // PartitionDateTimeRangePattern, steps 3-4.
  JS::ClippedTime y;
  if (!HandleDateTimeValue(cx, method, dateTimeFormat, end, &y)) {
    return false;
  }
  MOZ_ASSERT(y.isValid());

  mozilla::intl::DateTimeFormat* df =
      GetOrCreateDateTimeFormat(cx, dateTimeFormat, startKind);
  if (!df) {
    return false;
  }

  mozilla::intl::DateIntervalFormat* dif =
      GetOrCreateDateIntervalFormat(cx, dateTimeFormat, *df, startKind);
  if (!dif) {
    return false;
  }

  // Use the DateIntervalFormat to actually format the time range.
  return formatToParts
             ? FormatDateTimeRangeToParts(cx, df, dif, x, y, args.rval())
             : FormatDateTimeRange(cx, df, dif, x, y, args.rval());
}

bool js::TemporalObjectToLocaleString(JSContext* cx, const CallArgs& args,
                                      Handle<JSString*> required,
                                      Handle<JSString*> defaults,
                                      Handle<Value> toLocaleStringTimeZone) {
  MOZ_ASSERT(args.thisv().isObject());

  auto kind = ToDateTimeFormattable(args.thisv());
  MOZ_ASSERT(kind != DateTimeValueKind::Number);
  MOZ_ASSERT_IF(kind != DateTimeValueKind::TemporalZonedDateTime,
                toLocaleStringTimeZone.isUndefined());
  MOZ_ASSERT_IF(kind == DateTimeValueKind::TemporalZonedDateTime,
                toLocaleStringTimeZone.isString());

  Rooted<DateTimeFormatObject*> dateTimeFormat(
      cx, NewBuiltinClassInstance<DateTimeFormatObject>(cx));
  if (!dateTimeFormat) {
    return false;
  }

  Rooted<Value> thisValue(cx, ObjectValue(*dateTimeFormat));
  Rooted<Value> ignored(cx);
  if (!intl::InitializeDateTimeFormatObject(
          cx, dateTimeFormat, thisValue, args.get(0), args.get(1), required,
          defaults, toLocaleStringTimeZone, DateTimeFormatOptions::Standard,
          &ignored)) {
    return false;
  }
  MOZ_ASSERT(&ignored.toObject() == dateTimeFormat);

  JS::ClippedTime x;
  if (kind == DateTimeValueKind::TemporalZonedDateTime) {
    Rooted<ZonedDateTimeObject*> zonedDateTime(
        cx, &args.thisv().toObject().as<ZonedDateTimeObject>());
    if (!HandleDateTimeTemporalZonedDateTime(cx, dateTimeFormat, zonedDateTime,
                                             &x)) {
      return false;
    }
  } else {
    if (!HandleDateTimeValue(cx, "toLocaleString", dateTimeFormat, args.thisv(),
                             &x)) {
      return false;
    }
  }
  MOZ_ASSERT(x.isValid());

  auto* df = GetOrCreateDateTimeFormat(cx, dateTimeFormat, kind);
  if (!df) {
    return false;
  }

  return intl_FormatDateTime(cx, df, x, args.rval());
}
