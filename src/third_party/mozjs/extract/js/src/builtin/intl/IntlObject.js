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
 *   weekendStart:
 *     The day considered the beginning of a weekend for the resolved locale.
 *
 *   weekendEnd:
 *     The day considered the end of a weekend for the resolved locale.
 *
 * Days are encoded as integers in the range 1=Sunday to 7=Saturday.
 */
function Intl_getCalendarInfo(locales) {
    // 1. Let requestLocales be ? CanonicalizeLocaleList(locales).
    const requestedLocales = CanonicalizeLocaleList(locales);

    const DateTimeFormat = dateTimeFormatInternalProperties;

    // 2. Let localeData be %DateTimeFormat%.[[localeData]].
    const localeData = DateTimeFormat.localeData;

    // 3. Let localeOpt be a new Record.
    const localeOpt = new Record();

    // 4. Set localeOpt.[[localeMatcher]] to "best fit".
    localeOpt.localeMatcher = "best fit";

    // 5. Let r be ResolveLocale(%DateTimeFormat%.[[availableLocales]],
    //    requestedLocales, localeOpt,
    //    %DateTimeFormat%.[[relevantExtensionKeys]], localeData).
    const r = ResolveLocale(callFunction(DateTimeFormat.availableLocales, DateTimeFormat),
                            requestedLocales,
                            localeOpt,
                            DateTimeFormat.relevantExtensionKeys,
                            localeData);

    // 6. Let result be GetCalendarInfo(r.[[locale]]).
    const result = intl_GetCalendarInfo(r.locale);
    _DefineDataProperty(result, "calendar", r.ca);
    _DefineDataProperty(result, "locale", r.locale);

    // 7. Return result.
    return result;
}

/**
 * This function is a custom function in the style of the standard Intl.*
 * functions, that isn't part of any spec or proposal yet.
 * We want to use it internally to retrieve translated values from CLDR in
 * order to ensure they're aligned with what Intl API returns.
 *
 * This API may one day be a foundation for an ECMA402 API spec proposal.
 *
 * The function takes two arguments - locales which is a list of locale strings
 * and options which is an object with two optional properties:
 *
 *   keys:
 *     an Array of string values that are paths to individual terms
 *
 *   style:
 *     a String with a value "long", "short" or "narrow"
 *
 * It returns an object with properties:
 *
 *   locale:
 *     a negotiated locale string
 *
 *   style:
 *     negotiated style
 *
 *   values:
 *     A key-value pair list of requested keys and corresponding
 *     translated values
 *
 */
function Intl_getDisplayNames(locales, options) {
    // 1. Let requestLocales be ? CanonicalizeLocaleList(locales).
    const requestedLocales = CanonicalizeLocaleList(locales);

    // 2. If options is undefined, then
    if (options === undefined)
        // a. Let options be ObjectCreate(null).
        options = std_Object_create(null);
    // 3. Else,
    else
        // a. Let options be ? ToObject(options).
        options = ToObject(options);

    const DateTimeFormat = dateTimeFormatInternalProperties;

    // 4. Let localeData be %DateTimeFormat%.[[localeData]].
    const localeData = DateTimeFormat.localeData;

    // 5. Let localeOpt be a new Record.
    const localeOpt = new Record();

    // 6. Set localeOpt.[[localeMatcher]] to "best fit".
    localeOpt.localeMatcher = "best fit";

    // 7. Let r be ResolveLocale(%DateTimeFormat%.[[availableLocales]], requestedLocales, localeOpt,
    //    %DateTimeFormat%.[[relevantExtensionKeys]], localeData).
    const r = ResolveLocale(callFunction(DateTimeFormat.availableLocales, DateTimeFormat),
                          requestedLocales,
                          localeOpt,
                          DateTimeFormat.relevantExtensionKeys,
                          localeData);

    // 8. Let style be ? GetOption(options, "style", "string", « "long", "short", "narrow" », "long").
    const style = GetOption(options, "style", "string", ["long", "short", "narrow"], "long");

    // 9. Let keys be ? Get(options, "keys").
    let keys = options.keys;

    // 10. If keys is undefined,
    if (keys === undefined) {
        // a. Let keys be ArrayCreate(0).
        keys = [];
    } else if (!IsObject(keys)) {
        // 11. Else,
        //   a. If Type(keys) is not Object, throw a TypeError exception.
        ThrowTypeError(JSMSG_INVALID_KEYS_TYPE);
    }

    // 12. Let processedKeys be ArrayCreate(0).
    // (This really should be a List, but we use an Array here in order that
    // |intl_ComputeDisplayNames| may infallibly access the list's length via
    // |ArrayObject::length|.)
    let processedKeys = [];

    // 13. Let len be ? ToLength(? Get(keys, "length")).
    let len = ToLength(keys.length);

    // 14. Let i be 0.
    // 15. Repeat, while i < len
    for (let i = 0; i < len; i++) {
        // a. Let processedKey be ? ToString(? Get(keys, i)).
        // b. Perform ? CreateDataPropertyOrThrow(processedKeys, i, processedKey).
        _DefineDataProperty(processedKeys, i, ToString(keys[i]));
    }

    // 16. Let names be ? ComputeDisplayNames(r.[[locale]], style, processedKeys).
    const names = intl_ComputeDisplayNames(r.locale, style, processedKeys);

    // 17. Let values be ObjectCreate(%ObjectPrototype%).
    const values = {};

    // 18. Set i to 0.
    // 19. Repeat, while i < len
    for (let i = 0; i < len; i++) {
        // a. Let key be ? Get(processedKeys, i).
        const key = processedKeys[i];
        // b. Let name be ? Get(names, i).
        const name = names[i];
        // c. Assert: Type(name) is string.
        assert(typeof name === "string", "unexpected non-string value");
        // d. Assert: the length of name is greater than zero.
        assert(name.length > 0, "empty string value");
        // e. Perform ? DefinePropertyOrThrow(values, key, name).
        _DefineDataProperty(values, key, name);
    }

    // 20. Let options be ObjectCreate(%ObjectPrototype%).
    // 21. Perform ! DefinePropertyOrThrow(result, "locale", r.[[locale]]).
    // 22. Perform ! DefinePropertyOrThrow(result, "style", style).
    // 23. Perform ! DefinePropertyOrThrow(result, "values", values).
    const result = { locale: r.locale, style, values };

    // 24. Return result.
    return result;
}

function Intl_getLocaleInfo(locales) {
  const requestedLocales = CanonicalizeLocaleList(locales);

  // In the future, we may want to expose uloc_getAvailable and use it here.
  const DateTimeFormat = dateTimeFormatInternalProperties;
  const localeData = DateTimeFormat.localeData;

  const localeOpt = new Record();
  localeOpt.localeMatcher = "best fit";

  const r = ResolveLocale(callFunction(DateTimeFormat.availableLocales, DateTimeFormat),
                          requestedLocales,
                          localeOpt,
                          DateTimeFormat.relevantExtensionKeys,
                          localeData);

  return intl_GetLocaleInfo(r.locale);
}
