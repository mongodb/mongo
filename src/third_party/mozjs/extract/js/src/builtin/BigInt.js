/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if JS_HAS_INTL_API
/**
 * Format this BigInt object into a string, using the locale and formatting
 * options provided.
 *
 * Spec PR: https://github.com/tc39/ecma402/pull/236
 */
function BigInt_toLocaleString() {
    // Step 1. Note that valueOf enforces "thisBigIntValue" restrictions.
    var x = callFunction(std_BigInt_valueOf, this);

    var locales = arguments.length > 0 ? arguments[0] : undefined;
    var options = arguments.length > 1 ? arguments[1] : undefined;

    // Step 2.
    var numberFormat;
    if (locales === undefined && options === undefined) {
        // This cache only optimizes when no explicit locales and options
        // arguments were supplied.
        if (!intl_IsRuntimeDefaultLocale(numberFormatCache.runtimeDefaultLocale)) {
            numberFormatCache.numberFormat = intl_NumberFormat(locales, options);
            numberFormatCache.runtimeDefaultLocale = intl_RuntimeDefaultLocale();
        }
        numberFormat = numberFormatCache.numberFormat;
    } else {
        numberFormat = intl_NumberFormat(locales, options);
    }

    // Step 3.
    return intl_FormatNumber(numberFormat, x, /* formatToParts = */ false);
}
#endif  // JS_HAS_INTL_API
