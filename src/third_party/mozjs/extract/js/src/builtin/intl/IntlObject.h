/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_IntlObject_h
#define builtin_intl_IntlObject_h

#include "js/RootingAPI.h"
#include "js/TypeDecls.h"

namespace js {

extern const JSClass IntlClass;

/**
 * Returns a plain object with calendar information for a single valid locale
 * (callers must perform this validation).  The object will have these
 * properties:
 *
 *   firstDayOfWeek
 *     an integer in the range 1=Sunday to 7=Saturday indicating the day
 *     considered the first day of the week in calendars, e.g. 1 for en-US,
 *     2 for en-GB, 1 for bn-IN
 *   minDays
 *     an integer in the range of 1 to 7 indicating the minimum number
 *     of days required in the first week of the year, e.g. 1 for en-US,
 *     4 for de
 *   weekendStart
 *     an integer in the range 1=Sunday to 7=Saturday indicating the day
 *     considered the beginning of a weekend, e.g. 7 for en-US, 7 for en-GB,
 *     1 for bn-IN
 *   weekendEnd
 *     an integer in the range 1=Sunday to 7=Saturday indicating the day
 *     considered the end of a weekend, e.g. 1 for en-US, 1 for en-GB,
 *     1 for bn-IN (note that "weekend" is *not* necessarily two days)
 *
 * NOTE: "calendar" and "locale" properties are *not* added to the object.
 */
[[nodiscard]] extern bool intl_GetCalendarInfo(JSContext* cx, unsigned argc,
                                               JS::Value* vp);

/**
 * Returns a plain object with locale information for a single valid locale
 * (callers must perform this validation).  The object will have these
 * properties:
 *
 *   direction
 *     a string with a value "ltr" for left-to-right locale, and "rtl" for
 *     right-to-left locale.
 *   locale
 *     a BCP47 compilant locale string for the resolved locale.
 */
[[nodiscard]] extern bool intl_GetLocaleInfo(JSContext* cx, unsigned argc,
                                             JS::Value* vp);

/**
 * Returns an Array with CLDR-based fields display names.
 * The function takes three arguments:
 *
 *   locale
 *     BCP47 compliant locale string
 *   style
 *     A string with values: long or short or narrow
 *   keys
 *     An array or path-like strings that identify keys to be returned
 *     At the moment the following types of keys are supported:
 *
 *       'dates/fields/{year|month|week|day}'
 *       'dates/gregorian/months/{january|...|december}'
 *       'dates/gregorian/weekdays/{sunday|...|saturday}'
 *       'dates/gregorian/dayperiods/{am|pm}'
 *
 * Example:
 *
 * let info = intl_ComputeDisplayNames(
 *   'en-US',
 *   'long',
 *   [
 *     'dates/fields/year',
 *     'dates/gregorian/months/january',
 *     'dates/gregorian/weekdays/monday',
 *     'dates/gregorian/dayperiods/am',
 *   ]
 * );
 *
 * Returned value:
 *
 * [
 *   'year',
 *   'January',
 *   'Monday',
 *   'AM'
 * ]
 */
[[nodiscard]] extern bool intl_ComputeDisplayNames(JSContext* cx, unsigned argc,
                                                   JS::Value* vp);
/**
 * Compares a BCP 47 language tag against the locales in availableLocales and
 * returns the best available match -- or |undefined| if no match was found.
 * Uses the fallback mechanism of RFC 4647, section 3.4.
 *
 * The set of available locales consulted doesn't necessarily include the
 * default locale or any generalized forms of it (e.g. "de" is a more-general
 * form of "de-CH"). If you want to be sure to consider the default local and
 * its generalized forms (you usually will), pass the default locale as the
 * value of |defaultOrNull|; otherwise pass null.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.2.
 * Spec: RFC 4647, section 3.4.
 *
 * Usage: result = intl_BestAvailableLocale("Collator", locale, defaultOrNull)
 */
[[nodiscard]] extern bool intl_BestAvailableLocale(JSContext* cx, unsigned argc,
                                                   JS::Value* vp);

/**
 * Return the supported locale for the input locale if ICU supports that locale
 * (perhaps via fallback, e.g. supporting "de-CH" through "de" support implied
 * by a "de-DE" locale). Otherwise uses the last-ditch locale.
 *
 * Usage: result = intl_supportedLocaleOrFallback(locale)
 */
[[nodiscard]] extern bool intl_supportedLocaleOrFallback(JSContext* cx,
                                                         unsigned argc,
                                                         JS::Value* vp);

}  // namespace js

#endif /* builtin_intl_IntlObject_h */
