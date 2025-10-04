/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if JS_HAS_INTL_API
// This cache, once primed, has these properties:
//
//   locale:
//     The requested locale, either |undefined| or a string. Not necessarily a
//     valid and supported DateTimeFormat locale.
//   runtimeDefaultLocale:
//     Locale information provided by the embedding, guiding SpiderMonkey's
//     selection of a default locale.  See intl_RuntimeDefaultLocale(), whose
//     value controls the value returned by DefaultLocale() that's what's
//     *actually* used.
//   icuDefaultTimeZone:
//     Time zone information provided by ICU. See intl_defaultTimeZone(),
//     whose value controls the value returned by DefaultTimeZone() that's
//     what's *actually* used.
//   formatters:
//     A Record storing formatters consistent with the above locale and time
//     zone values, for use with the appropriate toLocale*String Date method
//     when called with the |locales| argument either |undefined| or a string,
//     and the |options| argument having the value |undefined|.
//
// The "formatters" Record has (some subset of) these properties, as determined
// by all values of the first argument passed to |GetCachedFormat|:
//
//   dateTimeFormat: for Date's toLocaleString operation
//   dateFormat: for Date's toLocaleDateString operation
//   timeFormat: for Date's toLocaleTimeString operation
//
// Using this cache, then, requires
// 1) verifying the requested locale is consistent with the cached value, then
// 2) verifying the current runtimeDefaultLocale/icuDefaultTimeZone are
//    consistent with cached values, then
// 3) seeing if the desired formatter is cached and returning it if so, or else
// 4) create the desired formatter and store and return it.
var dateTimeFormatCache = new_Record();

/**
 * Get a cached DateTimeFormat formatter object, created like so:
 *
 *   CreateDateTimeFormat(locale, undefined, required, defaults);
 *
 * |format| must be a key from the "formatters" Record described above.
 */
function GetCachedFormat(locale, format, required, defaults) {
  assert(
    locale === undefined || typeof locale === "string",
    "bad locale type"
  );
  assert(
    format === "dateTimeFormat" ||
      format === "dateFormat" ||
      format === "timeFormat",
    "unexpected format key: please update the comment by dateTimeFormatCache"
  );

  var formatters;
  if (
    locale !== dateTimeFormatCache.locale ||
    !intl_IsRuntimeDefaultLocale(dateTimeFormatCache.runtimeDefaultLocale) ||
    !intl_isDefaultTimeZone(dateTimeFormatCache.icuDefaultTimeZone)
  ) {
    formatters = dateTimeFormatCache.formatters = new_Record();
    dateTimeFormatCache.locale = locale;
    dateTimeFormatCache.runtimeDefaultLocale = intl_RuntimeDefaultLocale();
    dateTimeFormatCache.icuDefaultTimeZone = intl_defaultTimeZone();
  } else {
    formatters = dateTimeFormatCache.formatters;
  }

  var fmt = formatters[format];
  if (fmt === undefined) {
    fmt = formatters[format] = intl_CreateDateTimeFormat(locale, undefined, required, defaults);
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
  // Steps 1-2.
  var x = callFunction(ThisTimeValue, this, DATE_METHOD_LOCALE_STRING);
  if (Number_isNaN(x)) {
    return "Invalid Date";
  }

  // Steps 3-4.
  var locales = ArgumentsLength() ? GetArgument(0) : undefined;
  var options = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  // Step 5-6.
  var dateTimeFormat;
  if (
    (locales === undefined || typeof locales === "string") &&
    options === undefined
  ) {
    // This cache only optimizes when |options| isn't used.
    dateTimeFormat = GetCachedFormat(locales, "dateTimeFormat", "any", "all");
  } else {
    dateTimeFormat = intl_CreateDateTimeFormat(locales, options, "any", "all");
  }

  // Step 7.
  return intl_FormatDateTime(dateTimeFormat, x, /* formatToParts = */ false);
}

/**
 * Format this Date object into a date string, using the locale and formatting
 * options provided.
 *
 * Spec: ECMAScript Language Specification, 5.1 edition, 15.9.5.6.
 * Spec: ECMAScript Internationalization API Specification, 13.3.2.
 */
function Date_toLocaleDateString() {
  // Steps 1-2.
  var x = callFunction(ThisTimeValue, this, DATE_METHOD_LOCALE_DATE_STRING);
  if (Number_isNaN(x)) {
    return "Invalid Date";
  }

  // Steps 3-4.
  var locales = ArgumentsLength() ? GetArgument(0) : undefined;
  var options = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  // Step 5-6.
  var dateTimeFormat;
  if (
    (locales === undefined || typeof locales === "string") &&
    options === undefined
  ) {
    // This cache only optimizes when |options| isn't used.
    dateTimeFormat = GetCachedFormat(locales, "dateFormat", "date", "date");
  } else {
    dateTimeFormat = intl_CreateDateTimeFormat(locales, options, "date", "date");
  }

  // Step 7.
  return intl_FormatDateTime(dateTimeFormat, x, /* formatToParts = */ false);
}

/**
 * Format this Date object into a time string, using the locale and formatting
 * options provided.
 *
 * Spec: ECMAScript Language Specification, 5.1 edition, 15.9.5.7.
 * Spec: ECMAScript Internationalization API Specification, 13.3.3.
 */
function Date_toLocaleTimeString() {
  // Steps 1-2.
  var x = callFunction(ThisTimeValue, this, DATE_METHOD_LOCALE_TIME_STRING);
  if (Number_isNaN(x)) {
    return "Invalid Date";
  }

  // Steps 3-4.
  var locales = ArgumentsLength() ? GetArgument(0) : undefined;
  var options = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  // Step 5-6.
  var dateTimeFormat;
  if (
    (locales === undefined || typeof locales === "string") &&
    options === undefined
  ) {
    // This cache only optimizes when |options| isn't used.
    dateTimeFormat = GetCachedFormat(locales, "timeFormat", "time", "time");
  } else {
    dateTimeFormat = intl_CreateDateTimeFormat(locales, options, "time", "time");
  }

  // Step 7.
  return intl_FormatDateTime(dateTimeFormat, x, /* formatToParts = */ false);
}
#endif  // JS_HAS_INTL_API
