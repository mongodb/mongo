/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * 8.2.1 Intl.getCanonicalLocales ( locales )
 *
 * ES2017 Intl draft rev 947aa9a0c853422824a0c9510d8f09be3eb416b9
 */
function Intl_getCanonicalLocales(locales) {
  // Steps 1-2.
  return CanonicalizeLocaleList(locales);
}

/**
 * Intl.supportedValuesOf ( key )
 */
function Intl_supportedValuesOf(key) {
  // Step 1.
  key = ToString(key);

  // Steps 2-9.
  return intl_SupportedValuesOf(key);
}

/**
 * This function is a custom function in the style of the standard Intl.*
 * functions, that isn't part of any spec or proposal yet.
 *
 * Returns an object with the following properties:
 *   locale:
 *     The actual resolved locale.
 *
 *   calendar:
 *     The default calendar of the resolved locale.
 *
 *   firstDayOfWeek:
 *     The first day of the week for the resolved locale.
 *
 *   minDays:
 *     The minimum number of days in a week for the resolved locale.
 *
 *   weekend:
 *     The days of the week considered as the weekend for the resolved locale.
 *
 * Days are encoded as integers in the range 1=Monday to 7=Sunday.
 */
function Intl_getCalendarInfo(locales) {
  // 1. Let requestLocales be ? CanonicalizeLocaleList(locales).
  var requestedLocales = CanonicalizeLocaleList(locales);

  var DateTimeFormat = dateTimeFormatInternalProperties;

  // 2. Let localeData be %DateTimeFormat%.[[localeData]].
  var localeData = DateTimeFormat.localeData;

  // 3. Let localeOpt be a new Record.
  var localeOpt = new_Record();

  // 4. Set localeOpt.[[localeMatcher]] to "best fit".
  localeOpt.localeMatcher = "best fit";

  // 5. Let r be ResolveLocale(%DateTimeFormat%.[[availableLocales]],
  //    requestedLocales, localeOpt,
  //    %DateTimeFormat%.[[relevantExtensionKeys]], localeData).
  var r = ResolveLocale(
    "DateTimeFormat",
    requestedLocales,
    localeOpt,
    DateTimeFormat.relevantExtensionKeys,
    localeData
  );

  // 6. Let result be GetCalendarInfo(r.[[locale]]).
  var result = intl_GetCalendarInfo(r.locale);
  DefineDataProperty(result, "calendar", r.ca);
  DefineDataProperty(result, "locale", r.locale);

  // 7. Return result.
  return result;
}
