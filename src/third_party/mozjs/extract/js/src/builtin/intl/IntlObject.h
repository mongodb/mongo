/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_intl_IntlObject_h
#define builtin_intl_IntlObject_h

#include "js/TypeDecls.h"

namespace js {

extern const JSClass IntlClass;

/**
 * Returns a plain object with calendar information for a single valid locale
 * (callers must perform this validation).  The object will have these
 * properties:
 *
 *   firstDayOfWeek
 *     an integer in the range 1=Monday to 7=Sunday indicating the day
 *     considered the first day of the week in calendars, e.g. 7 for en-US,
 *     1 for en-GB, 7 for bn-IN
 *   minDays
 *     an integer in the range of 1 to 7 indicating the minimum number
 *     of days required in the first week of the year, e.g. 1 for en-US,
 *     4 for de
 *   weekend
 *     an array with values in the range 1=Monday to 7=Sunday indicating the
 *     days of the week considered as part of the weekend, e.g. [6, 7] for en-US
 *     and en-GB, [7] for bn-IN (note that "weekend" is *not* necessarily two
 *     days)
 *
 * NOTE: "calendar" and "locale" properties are *not* added to the object.
 */
[[nodiscard]] extern bool intl_GetCalendarInfo(JSContext* cx, unsigned argc,
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

/**
 * Returns the list of supported values for the given key. Throws a RangeError
 * if the key isn't one of {"calendar", "collation", "currency",
 * "numberingSystem", "timeZone", "unit"}.
 *
 * Usage: list = intl_SupportedValuesOf(key)
 */
[[nodiscard]] extern bool intl_SupportedValuesOf(JSContext* cx, unsigned argc,
                                                 JS::Value* vp);

}  // namespace js

#endif /* builtin_intl_IntlObject_h */
