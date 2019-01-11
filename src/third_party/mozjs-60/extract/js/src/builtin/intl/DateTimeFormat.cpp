/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Intl.DateTimeFormat implementation. */

#include "builtin/intl/DateTimeFormat.h"

#include "mozilla/Assertions.h"
#include "mozilla/Range.h"

#include "jsfriendapi.h"

#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/ICUStubs.h"
#include "builtin/intl/ScopedICUObject.h"
#include "builtin/intl/SharedIntlData.h"
#include "builtin/intl/TimeZoneDataGenerated.h"
#include "gc/FreeOp.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/Runtime.h"

#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"

using namespace js;

using JS::ClippedTime;
using JS::TimeClip;

using js::intl::CallICU;
using js::intl::DateTimeFormatOptions;
using js::intl::GetAvailableLocales;
using js::intl::IcuLocale;
using js::intl::INITIAL_CHAR_BUFFER_SIZE;
using js::intl::SharedIntlData;
using js::intl::StringsAreEqual;

const ClassOps DateTimeFormatObject::classOps_ = {
    nullptr, /* addProperty */
    nullptr, /* delProperty */
    nullptr, /* enumerate */
    nullptr, /* newEnumerate */
    nullptr, /* resolve */
    nullptr, /* mayResolve */
    DateTimeFormatObject::finalize
};

const Class DateTimeFormatObject::class_ = {
    js_Object_str,
    JSCLASS_HAS_RESERVED_SLOTS(DateTimeFormatObject::SLOT_COUNT) |
    JSCLASS_FOREGROUND_FINALIZE,
    &DateTimeFormatObject::classOps_
};

static bool
dateTimeFormat_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setString(cx->names().DateTimeFormat);
    return true;
}

static const JSFunctionSpec dateTimeFormat_static_methods[] = {
    JS_SELF_HOSTED_FN("supportedLocalesOf", "Intl_DateTimeFormat_supportedLocalesOf", 1, 0),
    JS_FS_END
};

static const JSFunctionSpec dateTimeFormat_methods[] = {
    JS_SELF_HOSTED_FN("resolvedOptions", "Intl_DateTimeFormat_resolvedOptions", 0, 0),
    JS_SELF_HOSTED_FN("formatToParts", "Intl_DateTimeFormat_formatToParts", 1, 0),
    JS_FN(js_toSource_str, dateTimeFormat_toSource, 0, 0),
    JS_FS_END
};

static const JSPropertySpec dateTimeFormat_properties[] = {
    JS_SELF_HOSTED_GET("format", "Intl_DateTimeFormat_format_get", 0),
    JS_STRING_SYM_PS(toStringTag, "Object", JSPROP_READONLY),
    JS_PS_END
};

/**
 * 12.2.1 Intl.DateTimeFormat([ locales [, options]])
 *
 * ES2017 Intl draft rev 94045d234762ad107a3d09bb6f7381a65f1a2f9b
 */
static bool
DateTimeFormat(JSContext* cx, const CallArgs& args, bool construct, DateTimeFormatOptions dtfOptions)
{
    // Step 1 (Handled by OrdinaryCreateFromConstructor fallback code).

    // Step 2 (Inlined 9.1.14, OrdinaryCreateFromConstructor).
    RootedObject proto(cx);
    if (!GetPrototypeFromBuiltinConstructor(cx, args, &proto))
        return false;

    if (!proto) {
        proto = GlobalObject::getOrCreateDateTimeFormatPrototype(cx, cx->global());
        if (!proto)
            return false;
    }

    Rooted<DateTimeFormatObject*> dateTimeFormat(cx);
    dateTimeFormat = NewObjectWithGivenProto<DateTimeFormatObject>(cx, proto);
    if (!dateTimeFormat)
        return false;

    dateTimeFormat->setReservedSlot(DateTimeFormatObject::INTERNALS_SLOT, NullValue());
    dateTimeFormat->setReservedSlot(DateTimeFormatObject::UDATE_FORMAT_SLOT,
                                    PrivateValue(nullptr));

    RootedValue thisValue(cx, construct ? ObjectValue(*dateTimeFormat) : args.thisv());
    HandleValue locales = args.get(0);
    HandleValue options = args.get(1);

    // Step 3.
    return intl::LegacyInitializeObject(cx, dateTimeFormat, cx->names().InitializeDateTimeFormat,
                                        thisValue, locales, options, dtfOptions, args.rval());
}

static bool
DateTimeFormat(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    return DateTimeFormat(cx, args, args.isConstructing(), DateTimeFormatOptions::Standard);
}

static bool
MozDateTimeFormat(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    // Don't allow to call mozIntl.DateTimeFormat as a function. That way we
    // don't need to worry how to handle the legacy initialization semantics
    // when applied on mozIntl.DateTimeFormat.
    if (!ThrowIfNotConstructing(cx, args, "mozIntl.DateTimeFormat"))
        return false;

    return DateTimeFormat(cx, args, true, DateTimeFormatOptions::EnableMozExtensions);
}

bool
js::intl_DateTimeFormat(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);
    MOZ_ASSERT(!args.isConstructing());
    // intl_DateTimeFormat is an intrinsic for self-hosted JavaScript, so it
    // cannot be used with "new", but it still has to be treated as a
    // constructor.
    return DateTimeFormat(cx, args, true, DateTimeFormatOptions::Standard);
}

void
js::DateTimeFormatObject::finalize(FreeOp* fop, JSObject* obj)
{
    MOZ_ASSERT(fop->onActiveCooperatingThread());

    const Value& slot =
        obj->as<DateTimeFormatObject>().getReservedSlot(DateTimeFormatObject::UDATE_FORMAT_SLOT);
    if (UDateFormat* df = static_cast<UDateFormat*>(slot.toPrivate()))
        udat_close(df);
}

JSObject*
js::CreateDateTimeFormatPrototype(JSContext* cx, JS::Handle<JSObject*> Intl,
                                  JS::Handle<GlobalObject*> global,
                                  JS::MutableHandle<JSObject*> constructor,
                                  DateTimeFormatOptions dtfOptions)
{
    RootedFunction ctor(cx);
    ctor = dtfOptions == DateTimeFormatOptions::EnableMozExtensions
           ? GlobalObject::createConstructor(cx, MozDateTimeFormat, cx->names().DateTimeFormat, 0)
           : GlobalObject::createConstructor(cx, DateTimeFormat, cx->names().DateTimeFormat, 0);
    if (!ctor)
        return nullptr;

    RootedObject proto(cx, GlobalObject::createBlankPrototype<PlainObject>(cx, global));
    if (!proto)
        return nullptr;

    if (!LinkConstructorAndPrototype(cx, ctor, proto))
        return nullptr;

    // 12.3.2
    if (!JS_DefineFunctions(cx, ctor, dateTimeFormat_static_methods))
        return nullptr;

    // 12.4.4 and 12.4.5
    if (!JS_DefineFunctions(cx, proto, dateTimeFormat_methods))
        return nullptr;

    // 12.4.2 and 12.4.3
    if (!JS_DefineProperties(cx, proto, dateTimeFormat_properties))
        return nullptr;

    // 8.1
    RootedValue ctorValue(cx, ObjectValue(*ctor));
    if (!DefineDataProperty(cx, Intl, cx->names().DateTimeFormat, ctorValue, 0))
        return nullptr;

    constructor.set(ctor);
    return proto;
}

bool
js::AddMozDateTimeFormatConstructor(JSContext* cx, JS::Handle<JSObject*> intl)
{
    Handle<GlobalObject*> global = cx->global();

    RootedObject mozDateTimeFormat(cx);
    JSObject* mozDateTimeFormatProto =
        CreateDateTimeFormatPrototype(cx, intl, global, &mozDateTimeFormat,
                                      DateTimeFormatOptions::EnableMozExtensions);
    return mozDateTimeFormatProto != nullptr;
}

bool
js::intl_DateTimeFormat_availableLocales(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 0);

    RootedValue result(cx);
    if (!GetAvailableLocales(cx, udat_countAvailable, udat_getAvailable, &result))
        return false;
    args.rval().set(result);
    return true;
}

static bool
DefaultCalendar(JSContext* cx, const JSAutoByteString& locale, MutableHandleValue rval)
{
    UErrorCode status = U_ZERO_ERROR;
    UCalendar* cal = ucal_open(nullptr, 0, locale.ptr(), UCAL_DEFAULT, &status);

    // This correctly handles nullptr |cal| when opening failed.
    ScopedICUObject<UCalendar, ucal_close> closeCalendar(cal);

    const char* calendar = ucal_getType(cal, &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
    }

    // ICU returns old-style keyword values; map them to BCP 47 equivalents
    JSString* str = JS_NewStringCopyZ(cx, uloc_toUnicodeLocaleType("ca", calendar));
    if (!str)
        return false;

    rval.setString(str);
    return true;
}

struct CalendarAlias
{
    const char* const calendar;
    const char* const alias;
};

const CalendarAlias calendarAliases[] = {
    { "islamic-civil", "islamicc" },
    { "ethioaa", "ethiopic-amete-alem" }
};

bool
js::intl_availableCalendars(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isString());

    JSAutoByteString locale(cx, args[0].toString());
    if (!locale)
        return false;

    RootedObject calendars(cx, NewDenseEmptyArray(cx));
    if (!calendars)
        return false;
    uint32_t index = 0;

    // We need the default calendar for the locale as the first result.
    RootedValue element(cx);
    if (!DefaultCalendar(cx, locale, &element))
        return false;

    if (!DefineDataElement(cx, calendars, index++, element))
        return false;

    // Now get the calendars that "would make a difference", i.e., not the default.
    UErrorCode status = U_ZERO_ERROR;
    UEnumeration* values = ucal_getKeywordValuesForLocale("ca", locale.ptr(), false, &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
    }
    ScopedICUObject<UEnumeration, uenum_close> toClose(values);

    uint32_t count = uenum_count(values, &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
    }

    for (; count > 0; count--) {
        const char* calendar = uenum_next(values, nullptr, &status);
        if (U_FAILURE(status)) {
            intl::ReportInternalError(cx);
            return false;
        }

        // ICU returns old-style keyword values; map them to BCP 47 equivalents
        calendar = uloc_toUnicodeLocaleType("ca", calendar);

        JSString* jscalendar = JS_NewStringCopyZ(cx, calendar);
        if (!jscalendar)
            return false;
        element = StringValue(jscalendar);
        if (!DefineDataElement(cx, calendars, index++, element))
            return false;

        // ICU doesn't return calendar aliases, append them here.
        for (const auto& calendarAlias : calendarAliases) {
            if (StringsAreEqual(calendar, calendarAlias.calendar)) {
                JSString* jscalendar = JS_NewStringCopyZ(cx, calendarAlias.alias);
                if (!jscalendar)
                    return false;
                element = StringValue(jscalendar);
                if (!DefineDataElement(cx, calendars, index++, element))
                    return false;
            }
        }
    }

    args.rval().setObject(*calendars);
    return true;
}

bool
js::intl_defaultCalendar(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isString());

    JSAutoByteString locale(cx, args[0].toString());
    if (!locale)
        return false;

    return DefaultCalendar(cx, locale, args.rval());
}

bool
js::intl_IsValidTimeZoneName(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isString());

    SharedIntlData& sharedIntlData = cx->runtime()->sharedIntlData.ref();

    RootedString timeZone(cx, args[0].toString());
    RootedAtom validatedTimeZone(cx);
    if (!sharedIntlData.validateTimeZoneName(cx, timeZone, &validatedTimeZone))
        return false;

    if (validatedTimeZone) {
        cx->markAtom(validatedTimeZone);
        args.rval().setString(validatedTimeZone);
    } else {
        args.rval().setNull();
    }

    return true;
}

bool
js::intl_canonicalizeTimeZone(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isString());

    SharedIntlData& sharedIntlData = cx->runtime()->sharedIntlData.ref();

    // Some time zone names are canonicalized differently by ICU -- handle
    // those first:
    RootedString timeZone(cx, args[0].toString());
    RootedAtom ianaTimeZone(cx);
    if (!sharedIntlData.tryCanonicalizeTimeZoneConsistentWithIANA(cx, timeZone, &ianaTimeZone))
        return false;

    if (ianaTimeZone) {
        cx->markAtom(ianaTimeZone);
        args.rval().setString(ianaTimeZone);
        return true;
    }

    AutoStableStringChars stableChars(cx);
    if (!stableChars.initTwoByte(cx, timeZone))
        return false;

    mozilla::Range<const char16_t> tzchars = stableChars.twoByteRange();

    JSString* str = CallICU(cx, [&tzchars](UChar* chars, uint32_t size, UErrorCode* status) {
        return ucal_getCanonicalTimeZoneID(tzchars.begin().get(), tzchars.length(),
                                           chars, size, nullptr, status);
    });
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

bool
js::intl_defaultTimeZone(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 0);

    // The current default might be stale, because JS::ResetTimeZone() doesn't
    // immediately update ICU's default time zone. So perform an update if
    // needed.
    js::ResyncICUDefaultTimeZone();

    JSString* str = CallICU(cx, ucal_getDefaultTimeZone);
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

bool
js::intl_defaultTimeZoneOffset(JSContext* cx, unsigned argc, Value* vp) {
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 0);

    UErrorCode status = U_ZERO_ERROR;
    const UChar* uTimeZone = nullptr;
    int32_t uTimeZoneLength = 0;
    const char* rootLocale = "";
    UCalendar* cal = ucal_open(uTimeZone, uTimeZoneLength, rootLocale, UCAL_DEFAULT, &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
    }
    ScopedICUObject<UCalendar, ucal_close> toClose(cal);

    int32_t offset = ucal_get(cal, UCAL_ZONE_OFFSET, &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
    }

    args.rval().setInt32(offset);
    return true;
}

bool
js::intl_isDefaultTimeZone(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);
    MOZ_ASSERT(args[0].isString() || args[0].isUndefined());

    // |undefined| is the default value when the Intl runtime caches haven't
    // yet been initialized. Handle it the same way as a cache miss.
    if (args[0].isUndefined()) {
        args.rval().setBoolean(false);
        return true;
    }

    // The current default might be stale, because JS::ResetTimeZone() doesn't
    // immediately update ICU's default time zone. So perform an update if
    // needed.
    js::ResyncICUDefaultTimeZone();

    Vector<char16_t, INITIAL_CHAR_BUFFER_SIZE> chars(cx);
    int32_t size = CallICU(cx, ucal_getDefaultTimeZone, chars);
    if (size < 0)
        return false;

    JSLinearString* str = args[0].toString()->ensureLinear(cx);
    if (!str)
        return false;

    bool equals;
    if (str->length() == size_t(size)) {
        JS::AutoCheckCannotGC nogc;
        equals = str->hasLatin1Chars()
                 ? EqualChars(str->latin1Chars(nogc), chars.begin(), str->length())
                 : EqualChars(str->twoByteChars(nogc), chars.begin(), str->length());
    } else {
        equals = false;
    }

    args.rval().setBoolean(equals);
    return true;
}

bool
js::intl_patternForSkeleton(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 2);
    MOZ_ASSERT(args[0].isString());
    MOZ_ASSERT(args[1].isString());

    JSAutoByteString locale(cx, args[0].toString());
    if (!locale)
        return false;

    AutoStableStringChars skeleton(cx);
    if (!skeleton.initTwoByte(cx, args[1].toString()))
        return false;

    mozilla::Range<const char16_t> skelChars = skeleton.twoByteRange();

    UErrorCode status = U_ZERO_ERROR;
    UDateTimePatternGenerator* gen = udatpg_open(IcuLocale(locale.ptr()), &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
    }
    ScopedICUObject<UDateTimePatternGenerator, udatpg_close> toClose(gen);

    JSString* str =
        CallICU(cx, [gen, &skelChars](UChar* chars, uint32_t size, UErrorCode* status) {
            return udatpg_getBestPattern(gen, skelChars.begin().get(), skelChars.length(),
                                         chars, size, status);
        });
    if (!str)
        return false;

    args.rval().setString(str);
    return true;
}

bool
js::intl_patternForStyle(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 4);
    MOZ_ASSERT(args[0].isString());

    JSAutoByteString locale(cx, args[0].toString());
    if (!locale)
        return false;

    UDateFormatStyle dateStyle = UDAT_NONE;
    UDateFormatStyle timeStyle = UDAT_NONE;

    if (args[1].isString()) {
        JSLinearString* dateStyleStr = args[1].toString()->ensureLinear(cx);
        if (!dateStyleStr)
            return false;

        if (StringEqualsAscii(dateStyleStr, "full"))
            dateStyle = UDAT_FULL;
        else if (StringEqualsAscii(dateStyleStr, "long"))
            dateStyle = UDAT_LONG;
        else if (StringEqualsAscii(dateStyleStr, "medium"))
            dateStyle = UDAT_MEDIUM;
        else if (StringEqualsAscii(dateStyleStr, "short"))
            dateStyle = UDAT_SHORT;
        else
            MOZ_ASSERT_UNREACHABLE("unexpected dateStyle");
    }

    if (args[2].isString()) {
        JSLinearString* timeStyleStr = args[2].toString()->ensureLinear(cx);
        if (!timeStyleStr)
            return false;

        if (StringEqualsAscii(timeStyleStr, "full"))
            timeStyle = UDAT_FULL;
        else if (StringEqualsAscii(timeStyleStr, "long"))
            timeStyle = UDAT_LONG;
        else if (StringEqualsAscii(timeStyleStr, "medium"))
            timeStyle = UDAT_MEDIUM;
        else if (StringEqualsAscii(timeStyleStr, "short"))
            timeStyle = UDAT_SHORT;
        else
            MOZ_ASSERT_UNREACHABLE("unexpected timeStyle");
    }

    AutoStableStringChars timeZone(cx);
    if (!timeZone.initTwoByte(cx, args[3].toString()))
        return false;

    mozilla::Range<const char16_t> timeZoneChars = timeZone.twoByteRange();

    UErrorCode status = U_ZERO_ERROR;
    UDateFormat* df = udat_open(timeStyle, dateStyle, IcuLocale(locale.ptr()),
                                timeZoneChars.begin().get(), timeZoneChars.length(),
                                nullptr, -1, &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
    }
    ScopedICUObject<UDateFormat, udat_close> toClose(df);

    JSString* str = CallICU(cx, [df](UChar* chars, uint32_t size, UErrorCode* status) {
        return udat_toPattern(df, false, chars, size, status);
    });
    if (!str)
        return false;
    args.rval().setString(str);
    return true;
}

/**
 * Returns a new UDateFormat with the locale and date-time formatting options
 * of the given DateTimeFormat.
 */
static UDateFormat*
NewUDateFormat(JSContext* cx, Handle<DateTimeFormatObject*> dateTimeFormat)
{
    RootedValue value(cx);

    RootedObject internals(cx, intl::GetInternalsObject(cx, dateTimeFormat));
    if (!internals)
       return nullptr;

    if (!GetProperty(cx, internals, internals, cx->names().locale, &value))
        return nullptr;
    JSAutoByteString locale(cx, value.toString());
    if (!locale)
        return nullptr;

    // We don't need to look at calendar and numberingSystem - they can only be
    // set via the Unicode locale extension and are therefore already set on
    // locale.

    if (!GetProperty(cx, internals, internals, cx->names().timeZone, &value))
        return nullptr;

    AutoStableStringChars timeZone(cx);
    if (!timeZone.initTwoByte(cx, value.toString()))
        return nullptr;

    mozilla::Range<const char16_t> timeZoneChars = timeZone.twoByteRange();

    if (!GetProperty(cx, internals, internals, cx->names().pattern, &value))
        return nullptr;

    AutoStableStringChars pattern(cx);
    if (!pattern.initTwoByte(cx, value.toString()))
        return nullptr;

    mozilla::Range<const char16_t> patternChars = pattern.twoByteRange();

    UErrorCode status = U_ZERO_ERROR;
    UDateFormat* df =
        udat_open(UDAT_PATTERN, UDAT_PATTERN, IcuLocale(locale.ptr()),
                  timeZoneChars.begin().get(), timeZoneChars.length(),
                  patternChars.begin().get(), patternChars.length(), &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return nullptr;
    }

    // ECMAScript requires the Gregorian calendar to be used from the beginning
    // of ECMAScript time.
    UCalendar* cal = const_cast<UCalendar*>(udat_getCalendar(df));
    ucal_setGregorianChange(cal, StartOfTime, &status);

    // An error here means the calendar is not Gregorian, so we don't care.

    return df;
}

static bool
intl_FormatDateTime(JSContext* cx, UDateFormat* df, ClippedTime x, MutableHandleValue result)
{
    MOZ_ASSERT(x.isValid());

    JSString* str = CallICU(cx, [df, x](UChar* chars, int32_t size, UErrorCode* status) {
        return udat_format(df, x.toDouble(), chars, size, nullptr, status);
    });
    if (!str)
        return false;

    result.setString(str);
    return true;
}

using FieldType = js::ImmutablePropertyNamePtr JSAtomState::*;

static FieldType
GetFieldTypeForFormatField(UDateFormatField fieldName)
{
    // See intl/icu/source/i18n/unicode/udat.h for a detailed field list.  This
    // switch is deliberately exhaustive: cases might have to be added/removed
    // if this code is compiled with a different ICU with more
    // UDateFormatField enum initializers.  Please guard such cases with
    // appropriate ICU version-testing #ifdefs, should cross-version divergence
    // occur.
    switch (fieldName) {
      case UDAT_ERA_FIELD:
        return &JSAtomState::era;
      case UDAT_YEAR_FIELD:
      case UDAT_YEAR_WOY_FIELD:
      case UDAT_EXTENDED_YEAR_FIELD:
      case UDAT_YEAR_NAME_FIELD:
        return &JSAtomState::year;

      case UDAT_MONTH_FIELD:
      case UDAT_STANDALONE_MONTH_FIELD:
        return &JSAtomState::month;

      case UDAT_DATE_FIELD:
      case UDAT_JULIAN_DAY_FIELD:
        return &JSAtomState::day;

      case UDAT_HOUR_OF_DAY1_FIELD:
      case UDAT_HOUR_OF_DAY0_FIELD:
      case UDAT_HOUR1_FIELD:
      case UDAT_HOUR0_FIELD:
        return &JSAtomState::hour;

      case UDAT_MINUTE_FIELD:
        return &JSAtomState::minute;

      case UDAT_SECOND_FIELD:
        return &JSAtomState::second;

      case UDAT_DAY_OF_WEEK_FIELD:
      case UDAT_STANDALONE_DAY_FIELD:
      case UDAT_DOW_LOCAL_FIELD:
      case UDAT_DAY_OF_WEEK_IN_MONTH_FIELD:
        return &JSAtomState::weekday;

      case UDAT_AM_PM_FIELD:
        return &JSAtomState::dayPeriod;

      case UDAT_TIMEZONE_FIELD:
        return &JSAtomState::timeZoneName;

      case UDAT_FRACTIONAL_SECOND_FIELD:
      case UDAT_DAY_OF_YEAR_FIELD:
      case UDAT_WEEK_OF_YEAR_FIELD:
      case UDAT_WEEK_OF_MONTH_FIELD:
      case UDAT_MILLISECONDS_IN_DAY_FIELD:
      case UDAT_TIMEZONE_RFC_FIELD:
      case UDAT_TIMEZONE_GENERIC_FIELD:
      case UDAT_QUARTER_FIELD:
      case UDAT_STANDALONE_QUARTER_FIELD:
      case UDAT_TIMEZONE_SPECIAL_FIELD:
      case UDAT_TIMEZONE_LOCALIZED_GMT_OFFSET_FIELD:
      case UDAT_TIMEZONE_ISO_FIELD:
      case UDAT_TIMEZONE_ISO_LOCAL_FIELD:
#ifndef U_HIDE_INTERNAL_API
      case UDAT_RELATED_YEAR_FIELD:
#endif
      case UDAT_AM_PM_MIDNIGHT_NOON_FIELD:
      case UDAT_FLEXIBLE_DAY_PERIOD_FIELD:
#ifndef U_HIDE_INTERNAL_API
      case UDAT_TIME_SEPARATOR_FIELD:
#endif
        // These fields are all unsupported.
        return nullptr;

#ifndef U_HIDE_DEPRECATED_API
      case UDAT_FIELD_COUNT:
        MOZ_ASSERT_UNREACHABLE("format field sentinel value returned by "
                               "iterator!");
#endif
    }

    MOZ_ASSERT_UNREACHABLE("unenumerated, undocumented format field returned "
                           "by iterator");
    return nullptr;
}

static bool
intl_FormatToPartsDateTime(JSContext* cx, UDateFormat* df, ClippedTime x,
                           MutableHandleValue result)
{
    MOZ_ASSERT(x.isValid());

    UErrorCode status = U_ZERO_ERROR;
    UFieldPositionIterator* fpositer = ufieldpositer_open(&status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
    }
    ScopedICUObject<UFieldPositionIterator, ufieldpositer_close> toClose(fpositer);

    RootedString overallResult(cx);
    overallResult = CallICU(cx, [df, x, fpositer](UChar* chars, int32_t size, UErrorCode* status) {
        return udat_formatForFields(df, x.toDouble(), chars, size, fpositer, status);
    });
    if (!overallResult)
        return false;

    RootedArrayObject partsArray(cx, NewDenseEmptyArray(cx));
    if (!partsArray)
        return false;

    if (overallResult->length() == 0) {
        // An empty string contains no parts, so avoid extra work below.
        result.setObject(*partsArray);
        return true;
    }

    size_t lastEndIndex = 0;

    uint32_t partIndex = 0;
    RootedObject singlePart(cx);
    RootedValue partType(cx);
    RootedValue val(cx);

    auto AppendPart = [&](FieldType type, size_t beginIndex, size_t endIndex) {
        singlePart = NewBuiltinClassInstance<PlainObject>(cx);
        if (!singlePart)
            return false;

        partType = StringValue(cx->names().*type);
        if (!DefineDataProperty(cx, singlePart, cx->names().type, partType))
            return false;

        JSLinearString* partSubstr =
            NewDependentString(cx, overallResult, beginIndex, endIndex - beginIndex);
        if (!partSubstr)
            return false;

        val = StringValue(partSubstr);
        if (!DefineDataProperty(cx, singlePart, cx->names().value, val))
            return false;

        val = ObjectValue(*singlePart);
        if (!DefineDataElement(cx, partsArray, partIndex, val))
            return false;

        lastEndIndex = endIndex;
        partIndex++;
        return true;
    };

    int32_t fieldInt, beginIndexInt, endIndexInt;
    while ((fieldInt = ufieldpositer_next(fpositer, &beginIndexInt, &endIndexInt)) >= 0) {
        MOZ_ASSERT(beginIndexInt >= 0);
        MOZ_ASSERT(endIndexInt >= 0);
        MOZ_ASSERT(beginIndexInt <= endIndexInt,
                   "field iterator returning invalid range");

        size_t beginIndex(beginIndexInt);
        size_t endIndex(endIndexInt);

        // Technically this isn't guaranteed.  But it appears true in pratice,
        // and http://bugs.icu-project.org/trac/ticket/12024 is expected to
        // correct the documentation lapse.
        MOZ_ASSERT(lastEndIndex <= beginIndex,
                   "field iteration didn't return fields in order start to "
                   "finish as expected");

        if (FieldType type = GetFieldTypeForFormatField(static_cast<UDateFormatField>(fieldInt))) {
            if (lastEndIndex < beginIndex) {
                if (!AppendPart(&JSAtomState::literal, lastEndIndex, beginIndex))
                    return false;
            }

            if (!AppendPart(type, beginIndex, endIndex))
                return false;
        }
    }

    // Append any final literal.
    if (lastEndIndex < overallResult->length()) {
        if (!AppendPart(&JSAtomState::literal, lastEndIndex, overallResult->length()))
            return false;
    }

    result.setObject(*partsArray);
    return true;
}

bool
js::intl_FormatDateTime(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 3);
    MOZ_ASSERT(args[0].isObject());
    MOZ_ASSERT(args[1].isNumber());
    MOZ_ASSERT(args[2].isBoolean());

    Rooted<DateTimeFormatObject*> dateTimeFormat(cx);
    dateTimeFormat = &args[0].toObject().as<DateTimeFormatObject>();

    ClippedTime x = TimeClip(args[1].toNumber());
    if (!x.isValid()) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_DATE_NOT_FINITE);
        return false;
    }

    // Obtain a cached UDateFormat object.
    void* priv =
        dateTimeFormat->getReservedSlot(DateTimeFormatObject::UDATE_FORMAT_SLOT).toPrivate();
    UDateFormat* df = static_cast<UDateFormat*>(priv);
    if (!df) {
        df = NewUDateFormat(cx, dateTimeFormat);
        if (!df)
            return false;
        dateTimeFormat->setReservedSlot(DateTimeFormatObject::UDATE_FORMAT_SLOT, PrivateValue(df));
    }

    // Use the UDateFormat to actually format the time stamp.
    return args[2].toBoolean()
           ? intl_FormatToPartsDateTime(cx, df, x, args.rval())
           : intl_FormatDateTime(cx, df, x, args.rval());
}
