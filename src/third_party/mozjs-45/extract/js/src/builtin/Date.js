/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*global intl_DateTimeFormat: false, */


// This cache, once primed, has these properties:
//
//   runtimeDefaultLocale:
//     Locale information provided by the embedding, guiding SpiderMonkey's
//     selection of a default locale.  See RuntimeDefaultLocale(), whose
//     value controls the value returned by DefaultLocale() that's what's
//     *actually* used.
//   localTZA:
//     The local time zone's adjustment from UTC.  See LocalTZA().
//   formatters:
//     A Record storing formatters consistent with the above
//     runtimeDefaultLocale/localTZA values, for use with the appropriate
//     ES6 toLocale*String Date method when called with its first two
//     arguments having the value |undefined|.
//
// The "formatters" Record has (some subset of) these properties, as determined
// by all values of the first argument passed to |GetCachedFormat|:
//
//   dateTimeFormat: for Date's toLocaleString operation
//   dateFormat: for Date's toLocaleDateString operation
//   timeFormat: for Date's toLocaleTimeString operation
//
// Using this cache, then, requires 1) verifying the current
// runtimeDefaultLocale/localTZA are consistent with cached values, then
// 2) seeing if the desired formatter is cached and returning it if so, or else
// 3) create the desired formatter and store and return it.
var dateTimeFormatCache = new Record();


/**
 * Get a cached DateTimeFormat formatter object, created like so:
 *
 *   var opts = ToDateTimeOptions(undefined, required, defaults);
 *   return new Intl.DateTimeFormat(undefined, opts);
 *
 * |format| must be a key from the "formatters" Record described above.
 */
function GetCachedFormat(format, required, defaults) {
    assert(format === "dateTimeFormat" ||
           format === "dateFormat" ||
           format === "timeFormat",
           "unexpected format key: please update the comment by " +
           "dateTimeFormatCache");

    var runtimeDefaultLocale = RuntimeDefaultLocale();
    var localTZA = LocalTZA();

    var formatters;
    if (dateTimeFormatCache.runtimeDefaultLocale !== runtimeDefaultLocale ||
        dateTimeFormatCache.localTZA !== localTZA)
    {
        formatters = dateTimeFormatCache.formatters = new Record();
        dateTimeFormatCache.runtimeDefaultLocale = runtimeDefaultLocale;
        dateTimeFormatCache.localTZA = localTZA;
    } else {
        formatters = dateTimeFormatCache.formatters;
    }

    var fmt = formatters[format];
    if (fmt === undefined) {
        var options = ToDateTimeOptions(undefined, required, defaults);
        fmt = formatters[format] = intl_DateTimeFormat(undefined, options);
    }

    return fmt;
}

/**
 * Format this Date object into a date and time string, using the locale and
 * formatting options provided.
 *
 * Spec: ECMAScript Language Specification, 5.1 edition, 15.9.5.5.
 * Spec: ECMAScript Internationalization API Specification, 13.3.1.
 */
function Date_toLocaleString() {
    // Steps 1-2.  Note that valueOf enforces "this time value" restrictions.
    var x = callFunction(std_Date_valueOf, this);
    if (Number_isNaN(x))
        return "Invalid Date";

    // Steps 3-4.
    var locales = arguments.length > 0 ? arguments[0] : undefined;
    var options = arguments.length > 1 ? arguments[1] : undefined;

    // Step 5-6.
    var dateTimeFormat;
    if (locales === undefined && options === undefined) {
        // This cache only optimizes for the old ES5 toLocaleString without
        // locales and options.
        dateTimeFormat = GetCachedFormat("dateTimeFormat", "any", "all");
    } else {
        options = ToDateTimeOptions(options, "any", "all");
        dateTimeFormat = intl_DateTimeFormat(locales, options);
    }

    // Step 7.
    return intl_FormatDateTime(dateTimeFormat, x);
}


/**
 * Format this Date object into a date string, using the locale and formatting
 * options provided.
 *
 * Spec: ECMAScript Language Specification, 5.1 edition, 15.9.5.6.
 * Spec: ECMAScript Internationalization API Specification, 13.3.2.
 */
function Date_toLocaleDateString() {
    // Steps 1-2.  Note that valueOf enforces "this time value" restrictions.
    var x = callFunction(std_Date_valueOf, this);
    if (Number_isNaN(x))
        return "Invalid Date";

    // Steps 3-4.
    var locales = arguments.length > 0 ? arguments[0] : undefined;
    var options = arguments.length > 1 ? arguments[1] : undefined;

    // Step 5-6.
    var dateTimeFormat;
    if (locales === undefined && options === undefined) {
        // This cache only optimizes for the old ES5 toLocaleDateString without
        // locales and options.
        dateTimeFormat = GetCachedFormat("dateFormat", "date", "date");
    } else {
        options = ToDateTimeOptions(options, "date", "date");
        dateTimeFormat = intl_DateTimeFormat(locales, options);
    }

    // Step 7.
    return intl_FormatDateTime(dateTimeFormat, x);
}


/**
 * Format this Date object into a time string, using the locale and formatting
 * options provided.
 *
 * Spec: ECMAScript Language Specification, 5.1 edition, 15.9.5.7.
 * Spec: ECMAScript Internationalization API Specification, 13.3.3.
 */
function Date_toLocaleTimeString() {
    // Steps 1-2.  Note that valueOf enforces "this time value" restrictions.
    var x = callFunction(std_Date_valueOf, this);
    if (Number_isNaN(x))
        return "Invalid Date";

    // Steps 3-4.
    var locales = arguments.length > 0 ? arguments[0] : undefined;
    var options = arguments.length > 1 ? arguments[1] : undefined;

    // Step 5-6.
    var dateTimeFormat;
    if (locales === undefined && options === undefined) {
        // This cache only optimizes for the old ES5 toLocaleTimeString without
        // locales and options.
        dateTimeFormat = GetCachedFormat("timeFormat", "time", "time");
    } else {
        options = ToDateTimeOptions(options, "time", "time");
        dateTimeFormat = intl_DateTimeFormat(locales, options);
    }

    // Step 7.
    return intl_FormatDateTime(dateTimeFormat, x);
}
