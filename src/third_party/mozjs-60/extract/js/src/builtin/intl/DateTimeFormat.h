/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_DateTimeFormat_h
#define builtin_intl_DateTimeFormat_h

#include "mozilla/Attributes.h"

#include "builtin/intl/CommonFunctions.h"
#include "builtin/SelfHostingDefines.h"
#include "js/Class.h"
#include "js/RootingAPI.h"
#include "vm/NativeObject.h"

namespace js {

class FreeOp;
class GlobalObject;

class DateTimeFormatObject : public NativeObject
{
  public:
    static const Class class_;

    static constexpr uint32_t INTERNALS_SLOT = 0;
    static constexpr uint32_t UDATE_FORMAT_SLOT = 1;
    static constexpr uint32_t SLOT_COUNT = 2;

    static_assert(INTERNALS_SLOT == INTL_INTERNALS_OBJECT_SLOT,
                  "INTERNALS_SLOT must match self-hosting define for internals object slot");

  private:
    static const ClassOps classOps_;

    static void finalize(FreeOp* fop, JSObject* obj);
};

extern JSObject*
CreateDateTimeFormatPrototype(JSContext* cx, JS::Handle<JSObject*> Intl,
                              JS::Handle<GlobalObject*> global,
                              JS::MutableHandle<JSObject*> constructor,
                              intl::DateTimeFormatOptions dtfOptions);

/**
 * Returns a new instance of the standard built-in DateTimeFormat constructor.
 * Self-hosted code cannot cache this constructor (as it does for others in
 * Utilities.js) because it is initialized after self-hosted code is compiled.
 *
 * Usage: dateTimeFormat = intl_DateTimeFormat(locales, options)
 */
extern MOZ_MUST_USE bool
intl_DateTimeFormat(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Returns an object indicating the supported locales for date and time
 * formatting by having a true-valued property for each such locale with the
 * canonicalized language tag as the property name. The object has no
 * prototype.
 *
 * Usage: availableLocales = intl_DateTimeFormat_availableLocales()
 */
extern MOZ_MUST_USE bool
intl_DateTimeFormat_availableLocales(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Returns an array with the calendar type identifiers per Unicode
 * Technical Standard 35, Unicode Locale Data Markup Language, for the
 * supported calendars for the given locale. The default calendar is
 * element 0.
 *
 * Usage: calendars = intl_availableCalendars(locale)
 */
extern MOZ_MUST_USE bool
intl_availableCalendars(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Returns the calendar type identifier per Unicode Technical Standard 35,
 * Unicode Locale Data Markup Language, for the default calendar for the given
 * locale.
 *
 * Usage: calendar = intl_defaultCalendar(locale)
 */
extern MOZ_MUST_USE bool
intl_defaultCalendar(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * 6.4.1 IsValidTimeZoneName ( timeZone )
 *
 * Verifies that the given string is a valid time zone name. If it is a valid
 * time zone name, its IANA time zone name is returned. Otherwise returns null.
 *
 * ES2017 Intl draft rev 4a23f407336d382ed5e3471200c690c9b020b5f3
 *
 * Usage: ianaTimeZone = intl_IsValidTimeZoneName(timeZone)
 */
extern MOZ_MUST_USE bool
intl_IsValidTimeZoneName(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Return the canonicalized time zone name. Canonicalization resolves link
 * names to their target time zones.
 *
 * Usage: ianaTimeZone = intl_canonicalizeTimeZone(timeZone)
 */
extern MOZ_MUST_USE bool
intl_canonicalizeTimeZone(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Return the default time zone name. The time zone name is not canonicalized.
 *
 * Usage: icuDefaultTimeZone = intl_defaultTimeZone()
 */
extern MOZ_MUST_USE bool
intl_defaultTimeZone(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Return the raw offset from GMT in milliseconds for the default time zone.
 *
 * Usage: defaultTimeZoneOffset = intl_defaultTimeZoneOffset()
 */
extern MOZ_MUST_USE bool
intl_defaultTimeZoneOffset(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Return true if the given string is the default time zone as returned by
 * intl_defaultTimeZone(). Otherwise return false.
 *
 * Usage: isIcuDefaultTimeZone = intl_isDefaultTimeZone(icuDefaultTimeZone)
 */
extern MOZ_MUST_USE bool
intl_isDefaultTimeZone(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Return a pattern in the date-time format pattern language of Unicode
 * Technical Standard 35, Unicode Locale Data Markup Language, for the
 * best-fit date-time format pattern corresponding to skeleton for the
 * given locale.
 *
 * Usage: pattern = intl_patternForSkeleton(locale, skeleton)
 */
extern MOZ_MUST_USE bool
intl_patternForSkeleton(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Return a pattern in the date-time format pattern language of Unicode
 * Technical Standard 35, Unicode Locale Data Markup Language, for the
 * best-fit date-time style for the given locale.
 * The function takes four arguments:
 *
 *   locale
 *     BCP47 compliant locale string
 *   dateStyle
 *     A string with values: full or long or medium or short, or `undefined`
 *   timeStyle
 *     A string with values: full or long or medium or short, or `undefined`
 *   timeZone
 *     IANA time zone name
 *
 * Date and time style categories map to CLDR time/date standard
 * format patterns.
 *
 * For the definition of a pattern string, see LDML 4.8:
 * http://unicode.org/reports/tr35/tr35-dates.html#Date_Format_Patterns
 *
 * If `undefined` is passed to `dateStyle` or `timeStyle`, the respective
 * portions of the pattern will not be included in the result.
 *
 * Usage: pattern = intl_patternForStyle(locale, dateStyle, timeStyle, timeZone)
 */
extern MOZ_MUST_USE bool
intl_patternForStyle(JSContext* cx, unsigned argc, JS::Value* vp);

/**
 * Returns a String value representing x (which must be a Number value)
 * according to the effective locale and the formatting options of the
 * given DateTimeFormat.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.3.2.
 *
 * Usage: formatted = intl_FormatDateTime(dateTimeFormat, x, formatToParts)
 */
extern MOZ_MUST_USE bool
intl_FormatDateTime(JSContext* cx, unsigned argc, JS::Value* vp);

} // namespace js

#endif /* builtin_intl_DateTimeFormat_h */
