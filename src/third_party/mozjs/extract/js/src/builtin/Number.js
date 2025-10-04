/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if JS_HAS_INTL_API
var numberFormatCache = new_Record();

/**
 * Format this Number object into a string, using the locale and formatting options
 * provided.
 *
 * Spec: ECMAScript Language Specification, 5.1 edition, 15.7.4.3.
 * Spec: ECMAScript Internationalization API Specification, 13.2.1.
 */
function Number_toLocaleString() {
  // Steps 1-2.
  var x = callFunction(ThisNumberValueForToLocaleString, this);

  // Steps 2-3.
  var locales = ArgumentsLength() ? GetArgument(0) : undefined;
  var options = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  // Step 4.
  var numberFormat;
  if (locales === undefined && options === undefined) {
    // This cache only optimizes for the old ES5 toLocaleString without
    // locales and options.
    if (!intl_IsRuntimeDefaultLocale(numberFormatCache.runtimeDefaultLocale)) {
      numberFormatCache.numberFormat = intl_NumberFormat(locales, options);
      numberFormatCache.runtimeDefaultLocale = intl_RuntimeDefaultLocale();
    }
    numberFormat = numberFormatCache.numberFormat;
  } else {
    numberFormat = intl_NumberFormat(locales, options);
  }

  // Step 5.
  return intl_FormatNumber(numberFormat, x, /* formatToParts = */ false);
}
#endif  // JS_HAS_INTL_API

// ES6 draft ES6 20.1.2.4
function Number_isFinite(num) {
  if (typeof num !== "number") {
    return false;
  }
  return num - num === 0;
}

// ES6 draft ES6 20.1.2.2
function Number_isNaN(num) {
  if (typeof num !== "number") {
    return false;
  }
  return num !== num;
}

// ES2021 draft rev 889f2f30cf554b7ed812c0984626db1c8a4997c7
// 20.1.2.3 Number.isInteger ( number )
function Number_isInteger(number) {
  // Step 1. (Inlined call to IsIntegralNumber)

  // 7.2.6 IsIntegralNumber, step 1.
  if (typeof number !== "number") {
    return false;
  }

  var integer = std_Math_trunc(number);

  // 7.2.6 IsIntegralNumber, steps 2-4.
  // |number - integer| ensures Infinity correctly returns false, because
  // |Infinity - Infinity| yields NaN.
  return number - integer === 0;
}

// ES2021 draft rev 889f2f30cf554b7ed812c0984626db1c8a4997c7
// 20.1.2.5 Number.isSafeInteger ( number )
function Number_isSafeInteger(number) {
  // Step 1. (Inlined call to IsIntegralNumber)

  // 7.2.6 IsIntegralNumber, step 1.
  if (typeof number !== "number") {
    return false;
  }

  var integer = std_Math_trunc(number);

  // 7.2.6 IsIntegralNumber, steps 2-4.
  // |number - integer| to handle the Infinity case correctly.
  if (number - integer !== 0) {
    return false;
  }

  // Steps 1.a, 2.
  // prettier-ignore
  return -((2 ** 53) - 1) <= integer && integer <= (2 ** 53) - 1;
}

function Global_isNaN(number) {
  return Number_isNaN(ToNumber(number));
}

function Global_isFinite(number) {
  return Number_isFinite(ToNumber(number));
}
