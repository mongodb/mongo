/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*global intl_DateTimeFormat: false, */


var dateTimeFormatCache = new Record();


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
        if (dateTimeFormatCache.dateTimeFormat === undefined) {
            options = ToDateTimeOptions(options, "any", "all");
            dateTimeFormatCache.dateTimeFormat = intl_DateTimeFormat(locales, options);
        }
        dateTimeFormat = dateTimeFormatCache.dateTimeFormat;
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
        if (dateTimeFormatCache.dateFormat === undefined) {
            options = ToDateTimeOptions(options, "date", "date");
            dateTimeFormatCache.dateFormat = intl_DateTimeFormat(locales, options);
        }
        dateTimeFormat = dateTimeFormatCache.dateFormat;
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
        if (dateTimeFormatCache.timeFormat === undefined) {
            options = ToDateTimeOptions(options, "time", "time");
            dateTimeFormatCache.timeFormat = intl_DateTimeFormat(locales, options);
        }
        dateTimeFormat = dateTimeFormatCache.timeFormat;
    } else {
        options = ToDateTimeOptions(options, "time", "time");
        dateTimeFormat = intl_DateTimeFormat(locales, options);
    }

    // Step 7.
    return intl_FormatDateTime(dateTimeFormat, x);
}
