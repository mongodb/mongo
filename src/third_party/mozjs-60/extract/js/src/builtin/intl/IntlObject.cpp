/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implementation of the Intl object and its non-constructor properties. */

#include "builtin/intl/IntlObject.h"

#include "mozilla/Assertions.h"
#include "mozilla/Likely.h"
#include "mozilla/Range.h"

#include "jsapi.h"

#include "builtin/intl/Collator.h"
#include "builtin/intl/CommonFunctions.h"
#include "builtin/intl/DateTimeFormat.h"
#include "builtin/intl/ICUStubs.h"
#include "builtin/intl/NumberFormat.h"
#include "builtin/intl/PluralRules.h"
#include "builtin/intl/ScopedICUObject.h"
#include "js/Class.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/JSObject.h"
#include "vm/StringType.h"

#include "vm/JSObject-inl.h"

using namespace js;

using mozilla::Range;
using mozilla::RangedPtr;

using js::intl::CallICU;
using js::intl::DateTimeFormatOptions;
using js::intl::IcuLocale;

/******************** Intl ********************/

bool
js::intl_GetCalendarInfo(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);

    JSAutoByteString locale(cx, args[0].toString());
    if (!locale)
        return false;

    UErrorCode status = U_ZERO_ERROR;
    const UChar* uTimeZone = nullptr;
    int32_t uTimeZoneLength = 0;
    UCalendar* cal = ucal_open(uTimeZone, uTimeZoneLength, locale.ptr(), UCAL_DEFAULT, &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
    }
    ScopedICUObject<UCalendar, ucal_close> toClose(cal);

    RootedObject info(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!info)
        return false;

    RootedValue v(cx);
    int32_t firstDayOfWeek = ucal_getAttribute(cal, UCAL_FIRST_DAY_OF_WEEK);
    v.setInt32(firstDayOfWeek);

    if (!DefineDataProperty(cx, info, cx->names().firstDayOfWeek, v))
        return false;

    int32_t minDays = ucal_getAttribute(cal, UCAL_MINIMAL_DAYS_IN_FIRST_WEEK);
    v.setInt32(minDays);
    if (!DefineDataProperty(cx, info, cx->names().minDays, v))
        return false;

    UCalendarWeekdayType prevDayType = ucal_getDayOfWeekType(cal, UCAL_SATURDAY, &status);
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
                // At the time this code was added, ICU apparently never behaves this way,
                // so just throw, so that users will report a bug and we can decide what to
                // do.
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

    if (!DefineDataProperty(cx, info, cx->names().weekendStart, weekendStart))
        return false;

    if (!DefineDataProperty(cx, info, cx->names().weekendEnd, weekendEnd))
        return false;

    args.rval().setObject(*info);
    return true;
}

static void
ReportBadKey(JSContext* cx, const Range<const JS::Latin1Char>& range)
{
    JS_ReportErrorNumberLatin1(cx, GetErrorMessage, nullptr, JSMSG_INVALID_KEY,
                               range.begin().get());
}

static void
ReportBadKey(JSContext* cx, const Range<const char16_t>& range)
{
    JS_ReportErrorNumberUC(cx, GetErrorMessage, nullptr, JSMSG_INVALID_KEY,
                           range.begin().get());
}

template<typename ConstChar>
static bool
MatchPart(RangedPtr<ConstChar> iter, const RangedPtr<ConstChar> end,
          const char* part, size_t partlen)
{
    for (size_t i = 0; i < partlen; iter++, i++) {
        if (iter == end || *iter != part[i])
            return false;
    }

    return true;
}

template<typename ConstChar, size_t N>
inline bool
MatchPart(RangedPtr<ConstChar>* iter, const RangedPtr<ConstChar> end, const char (&part)[N])
{
    if (!MatchPart(*iter, end, part, N - 1))
        return false;

    *iter += N - 1;
    return true;
}

enum class DisplayNameStyle
{
    Narrow,
    Short,
    Long,
};

template<typename ConstChar>
static JSString*
ComputeSingleDisplayName(JSContext* cx, UDateFormat* fmt, UDateTimePatternGenerator* dtpg,
                         DisplayNameStyle style, const Range<ConstChar>& pattern)
{
    RangedPtr<ConstChar> iter = pattern.begin();
    const RangedPtr<ConstChar> end = pattern.end();

    auto MatchSlash = [cx, pattern, &iter, end]() {
        if (MOZ_LIKELY(iter != end && *iter == '/')) {
            iter++;
            return true;
        }

        ReportBadKey(cx, pattern);
        return false;
    };

    if (!MatchPart(&iter, end, "dates")) {
        ReportBadKey(cx, pattern);
        return nullptr;
    }

    if (!MatchSlash())
        return nullptr;

    if (MatchPart(&iter, end, "fields")) {
        if (!MatchSlash())
            return nullptr;

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
            ReportBadKey(cx, pattern);
            return nullptr;
        }

        // This part must be the final part with no trailing data.
        if (iter != end) {
            ReportBadKey(cx, pattern);
            return nullptr;
        }

        int32_t resultSize;
        const UChar* value = udatpg_getAppendItemName(dtpg, fieldType, &resultSize);
        MOZ_ASSERT(resultSize >= 0);

        return NewStringCopyN<CanGC>(cx, value, size_t(resultSize));
    }

    if (MatchPart(&iter, end, "gregorian")) {
        if (!MatchSlash())
            return nullptr;

        UDateFormatSymbolType symbolType;
        int32_t index;

        if (MatchPart(&iter, end, "months")) {
            if (!MatchSlash())
                return nullptr;

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
                ReportBadKey(cx, pattern);
                return nullptr;
            }
        } else if (MatchPart(&iter, end, "weekdays")) {
            if (!MatchSlash())
                return nullptr;

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
                ReportBadKey(cx, pattern);
                return nullptr;
            }
        } else if (MatchPart(&iter, end, "dayperiods")) {
            if (!MatchSlash())
                return nullptr;

            symbolType = UDAT_AM_PMS;

            if (MatchPart(&iter, end, "am")) {
                index = UCAL_AM;
            } else if (MatchPart(&iter, end, "pm")) {
                index = UCAL_PM;
            } else {
                ReportBadKey(cx, pattern);
                return nullptr;
            }
        } else {
            ReportBadKey(cx, pattern);
            return nullptr;
        }

        // This part must be the final part with no trailing data.
        if (iter != end) {
            ReportBadKey(cx, pattern);
            return nullptr;
        }

        return CallICU(cx, [fmt, symbolType, index](UChar* chars, int32_t size,
                                                    UErrorCode* status)
        {
            return udat_getSymbols(fmt, symbolType, index, chars, size, status);
        });
    }

    ReportBadKey(cx, pattern);
    return nullptr;
}

bool
js::intl_ComputeDisplayNames(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 3);

    // 1. Assert: locale is a string.
    RootedString str(cx, args[0].toString());
    JSAutoByteString locale;
    if (!locale.encodeUtf8(cx, str))
        return false;

    // 2. Assert: style is a string.
    DisplayNameStyle dnStyle;
    {
        JSLinearString* style = args[1].toString()->ensureLinear(cx);
        if (!style)
            return false;

        if (StringEqualsAscii(style, "narrow")) {
            dnStyle = DisplayNameStyle::Narrow;
        } else if (StringEqualsAscii(style, "short")) {
            dnStyle = DisplayNameStyle::Short;
        } else {
            MOZ_ASSERT(StringEqualsAscii(style, "long"));
            dnStyle = DisplayNameStyle::Long;
        }
    }

    // 3. Assert: keys is an Array.
    RootedArrayObject keys(cx, &args[2].toObject().as<ArrayObject>());
    if (!keys)
        return false;

    // 4. Let result be ArrayCreate(0).
    RootedArrayObject result(cx, NewDenseUnallocatedArray(cx, keys->length()));
    if (!result)
        return false;

    UErrorCode status = U_ZERO_ERROR;

    UDateFormat* fmt =
        udat_open(UDAT_DEFAULT, UDAT_DEFAULT, IcuLocale(locale.ptr()),
        nullptr, 0, nullptr, 0, &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
    }
    ScopedICUObject<UDateFormat, udat_close> datToClose(fmt);

    // UDateTimePatternGenerator will be needed for translations of date and
    // time fields like "month", "week", "day" etc.
    UDateTimePatternGenerator* dtpg = udatpg_open(IcuLocale(locale.ptr()), &status);
    if (U_FAILURE(status)) {
        intl::ReportInternalError(cx);
        return false;
    }
    ScopedICUObject<UDateTimePatternGenerator, udatpg_close> datPgToClose(dtpg);

    // 5. For each element of keys,
    RootedString keyValStr(cx);
    RootedValue v(cx);
    for (uint32_t i = 0; i < keys->length(); i++) {
        if (!GetElement(cx, keys, keys, i, &v))
            return false;

        keyValStr = v.toString();

        AutoStableStringChars stablePatternChars(cx);
        if (!stablePatternChars.init(cx, keyValStr))
            return false;

        // 5.a. Perform an implementation dependent algorithm to map a key to a
        //      corresponding display name.
        JSString* displayName =
            stablePatternChars.isLatin1()
            ? ComputeSingleDisplayName(cx, fmt, dtpg, dnStyle, stablePatternChars.latin1Range())
            : ComputeSingleDisplayName(cx, fmt, dtpg, dnStyle, stablePatternChars.twoByteRange());
        if (!displayName)
            return false;

        // 5.b. Append the result string to result.
        v.setString(displayName);
        if (!DefineDataElement(cx, result, i, v))
            return false;
    }

    // 6. Return result.
    args.rval().setObject(*result);
    return true;
}

bool
js::intl_GetLocaleInfo(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    MOZ_ASSERT(args.length() == 1);

    JSAutoByteString locale(cx, args[0].toString());
    if (!locale)
        return false;

    RootedObject info(cx, NewBuiltinClassInstance<PlainObject>(cx));
    if (!info)
        return false;

    if (!DefineDataProperty(cx, info, cx->names().locale, args[0]))
        return false;

    bool rtl = uloc_isRightToLeft(IcuLocale(locale.ptr()));

    RootedValue dir(cx, StringValue(rtl ? cx->names().rtl : cx->names().ltr));

    if (!DefineDataProperty(cx, info, cx->names().direction, dir))
        return false;

    args.rval().setObject(*info);
    return true;
}

const Class js::IntlClass = {
    js_Object_str,
    JSCLASS_HAS_CACHED_PROTO(JSProto_Intl)
};

static bool
intl_toSource(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    args.rval().setString(cx->names().Intl);
    return true;
}

static const JSFunctionSpec intl_static_methods[] = {
    JS_FN(js_toSource_str,  intl_toSource,        0, 0),
    JS_SELF_HOSTED_FN("getCanonicalLocales", "Intl_getCanonicalLocales", 1, 0),
    JS_FS_END
};

/**
 * Initializes the Intl Object and its standard built-in properties.
 * Spec: ECMAScript Internationalization API Specification, 8.0, 8.1
 */
/* static */ bool
GlobalObject::initIntlObject(JSContext* cx, Handle<GlobalObject*> global)
{
    RootedObject proto(cx, GlobalObject::getOrCreateObjectPrototype(cx, global));
    if (!proto)
        return false;

    // The |Intl| object is just a plain object with some "static" function
    // properties and some constructor properties.
    RootedObject intl(cx, NewObjectWithGivenProto(cx, &IntlClass, proto, SingletonObject));
    if (!intl)
        return false;

    // Add the static functions.
    if (!JS_DefineFunctions(cx, intl, intl_static_methods))
        return false;

    // Add the constructor properties, computing and returning the relevant
    // prototype objects needed below.
    RootedObject collatorProto(cx, CreateCollatorPrototype(cx, intl, global));
    if (!collatorProto)
        return false;
    RootedObject dateTimeFormatProto(cx), dateTimeFormat(cx);
    dateTimeFormatProto = CreateDateTimeFormatPrototype(cx, intl, global, &dateTimeFormat,
                                                        DateTimeFormatOptions::Standard);
    if (!dateTimeFormatProto)
        return false;
    RootedObject numberFormatProto(cx), numberFormat(cx);
    numberFormatProto = CreateNumberFormatPrototype(cx, intl, global, &numberFormat);
    if (!numberFormatProto)
        return false;
    RootedObject pluralRulesProto(cx, CreatePluralRulesPrototype(cx, intl, global));
    if (!pluralRulesProto)
        return false;

    // The |Intl| object is fully set up now, so define the global property.
    RootedValue intlValue(cx, ObjectValue(*intl));
    if (!DefineDataProperty(cx, global, cx->names().Intl, intlValue, JSPROP_RESOLVING))
        return false;

    // Now that the |Intl| object is successfully added, we can OOM-safely fill
    // in all relevant reserved global slots.

    // Cache the various prototypes, for use in creating instances of these
    // objects with the proper [[Prototype]] as "the original value of
    // |Intl.Collator.prototype|" and similar.  For builtin classes like
    // |String.prototype| we have |JSProto_*| that enables
    // |getPrototype(JSProto_*)|, but that has global-object-property-related
    // baggage we don't need or want, so we use one-off reserved slots.
    global->setReservedSlot(COLLATOR_PROTO, ObjectValue(*collatorProto));
    global->setReservedSlot(DATE_TIME_FORMAT, ObjectValue(*dateTimeFormat));
    global->setReservedSlot(DATE_TIME_FORMAT_PROTO, ObjectValue(*dateTimeFormatProto));
    global->setReservedSlot(NUMBER_FORMAT, ObjectValue(*numberFormat));
    global->setReservedSlot(NUMBER_FORMAT_PROTO, ObjectValue(*numberFormatProto));
    global->setReservedSlot(PLURAL_RULES_PROTO, ObjectValue(*pluralRulesProto));

    // Also cache |Intl| to implement spec language that conditions behavior
    // based on values being equal to "the standard built-in |Intl| object".
    // Use |setConstructor| to correspond with |JSProto_Intl|.
    //
    // XXX We should possibly do a one-off reserved slot like above.
    global->setConstructor(JSProto_Intl, ObjectValue(*intl));
    return true;
}

JSObject*
js::InitIntlClass(JSContext* cx, HandleObject obj)
{
    Handle<GlobalObject*> global = obj.as<GlobalObject>();
    if (!GlobalObject::initIntlObject(cx, global))
        return nullptr;

    return &global->getConstructor(JSProto_Intl).toObject();
}
