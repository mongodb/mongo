/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*global intl_NumberFormat: false, */


var numberFormatCache = new Record();


/**
 * Format this Number object into a string, using the locale and formatting options
 * provided.
 *
 * Spec: ECMAScript Language Specification, 5.1 edition, 15.7.4.3.
 * Spec: ECMAScript Internationalization API Specification, 13.2.1.
 */
function Number_toLocaleString() {
    // Steps 1-2.  Note that valueOf enforces "this Number value" restrictions.
    var x = callFunction(std_Number_valueOf, this);

    // Steps 2-3.
    var locales = arguments.length > 0 ? arguments[0] : undefined;
    var options = arguments.length > 1 ? arguments[1] : undefined;

    // Step 4.
    var numberFormat;
    if (locales === undefined && options === undefined) {
        // This cache only optimizes for the old ES5 toLocaleString without
        // locales and options.
        if (!IsRuntimeDefaultLocale(numberFormatCache.runtimeDefaultLocale)) {
            numberFormatCache.numberFormat = intl_NumberFormat(locales, options);
            numberFormatCache.runtimeDefaultLocale = RuntimeDefaultLocale();
        }
        numberFormat = numberFormatCache.numberFormat;
    } else {
        numberFormat = intl_NumberFormat(locales, options);
    }

    // Step 5.
    return intl_FormatNumber(numberFormat, x, /* formatToParts = */ false);
}

// ES6 draft ES6 20.1.2.4
function Number_isFinite(num) {
    if (typeof num !== "number")
        return false;
    return num - num === 0;
}

// ES6 draft ES6 20.1.2.2
function Number_isNaN(num) {
    if (typeof num !== "number")
        return false;
    return num !== num;
}

// ES2018 draft rev f83aa38282c2a60c6916ebc410bfdf105a0f6a54
// 20.1.2.3 Number.isInteger ( number )
function Number_isInteger(number) {
    // Step 1.
    if (typeof number !== "number")
        return false;

    // -(2**31) is an int32, but abs(-(2**31)) isn't. Avoid bailouts below by
    // special casing it here.
    if (number === -(2 ** 31))
        return true;

    // Step 3.
    // Note: We're using abs + floor instead ceil to workaround bug 1379626.
    var absNumber = std_Math_abs(number);
    var integer = std_Math_floor(absNumber);

    // Steps 2, 4.
    if (absNumber - integer !== 0)
        return false;

    // Step 5.
    return true;
}

// ES2018 draft rev f83aa38282c2a60c6916ebc410bfdf105a0f6a54
// 20.1.2.5 Number.isSafeInteger ( number )
function Number_isSafeInteger(number) {
    // Step 1.
    if (typeof number !== "number")
        return false;

    // -(2**31) is an int32, but abs(-(2**31)) isn't. Avoid bailouts below by
    // special casing it here.
    if (number === -(2 ** 31))
        return true;

    // Step 3.
    var absNumber = std_Math_abs(number);
    var integer = std_Math_floor(absNumber);

    // Steps 2, 4.
    if (absNumber - integer !== 0)
        return false;

    // Step 5.
    if (integer <= (2 ** 53) - 1)
        return true;

    // Step 6.
    return false;
}

function Global_isNaN(number) {
    return Number_isNaN(ToNumber(number));
}

function Global_isFinite(number) {
    return Number_isFinite(ToNumber(number));
}
