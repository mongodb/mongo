/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Portions Copyright Norbert Lindenberg 2011-2012. */

/**
 * 11.1.2 CreateDateTimeFormat ( newTarget, locales, options, required, defaults [ , toLocaleStringTimeZone ] )
 *
 * Compute an internal properties object from |lazyDateTimeFormatData|.
 */
function resolveDateTimeFormatInternals(lazyDateTimeFormatData) {
  assert(IsObject(lazyDateTimeFormatData), "lazy data not an object?");

  // Lazy DateTimeFormat data has the following structure:
  //
  //   {
  //     requestedLocales: List of locales,
  //
  //     localeOpt: // *first* opt computed in InitializeDateTimeFormat
  //       {
  //         localeMatcher: "lookup" / "best fit",
  //
  //         ca: string matching a Unicode extension type, // optional
  //
  //         nu: string matching a Unicode extension type, // optional
  //
  //         hc: "h11" / "h12" / "h23" / "h24", // optional
  //       }
  //
  //     timeZone: IANA time zone name or a normalized time zone offset string,
  //
  //     formatOptions: // *second* opt computed in InitializeDateTimeFormat
  //       {
  //         // all the properties/values listed in Table 3
  //         // (weekday, era, year, month, day, &c.)
  //
  //         hour12: true / false,  // optional
  //       }
  //
  //     formatMatcher: "basic" / "best fit",
  //
  //     dateStyle: "full" / "long" / "medium" / "short" / undefined,
  //
  //     timeStyle: "full" / "long" / "medium" / "short" / undefined,
  //
  //     patternOption:
  //       String representing LDML Date Format pattern or undefined
  //   }
  //
  // Note that lazy data is only installed as a final step of initialization,
  // so every DateTimeFormat lazy data object has *all* these properties,
  // never a subset of them.

  var internalProps = std_Object_create(null);

  var DateTimeFormat = dateTimeFormatInternalProperties;

  // Compute effective locale.

  // Step 17.
  var localeData = DateTimeFormat.localeData;

  // Step 18.
  var r = ResolveLocale(
    "DateTimeFormat",
    lazyDateTimeFormatData.requestedLocales,
    lazyDateTimeFormatData.localeOpt,
    DateTimeFormat.relevantExtensionKeys,
    localeData
  );

  // Steps 19-22.
  internalProps.locale = r.locale;
  internalProps.calendar = r.ca;
  internalProps.numberingSystem = r.nu;

  // Step 34. (Reordered)
  var formatOptions = lazyDateTimeFormatData.formatOptions;

  // Steps 23-29.
  //
  // Copy the hourCycle setting, if present, to the format options. But
  // only do this if no hour12 option is present, because the latter takes
  // precedence over hourCycle.
  if (r.hc !== null && formatOptions.hour12 === undefined) {
    formatOptions.hourCycle = r.hc;
  }

  // Step 33.
  internalProps.timeZone = lazyDateTimeFormatData.timeZone;

  // Steps 45-50, more or less.
  if (lazyDateTimeFormatData.patternOption !== undefined) {
    internalProps.pattern = lazyDateTimeFormatData.patternOption;
  } else if (
    lazyDateTimeFormatData.dateStyle !== undefined ||
    lazyDateTimeFormatData.timeStyle !== undefined
  ) {
    internalProps.hourCycle = formatOptions.hourCycle;
    internalProps.hour12 = formatOptions.hour12;
    internalProps.dateStyle = lazyDateTimeFormatData.dateStyle;
    internalProps.timeStyle = lazyDateTimeFormatData.timeStyle;
  } else {
    internalProps.required = lazyDateTimeFormatData.required;
    internalProps.defaults = lazyDateTimeFormatData.defaults;
    internalProps.hourCycle = formatOptions.hourCycle;
    internalProps.hour12 = formatOptions.hour12;
    internalProps.weekday = formatOptions.weekday;
    internalProps.era = formatOptions.era;
    internalProps.year = formatOptions.year;
    internalProps.month = formatOptions.month;
    internalProps.day = formatOptions.day;
    internalProps.dayPeriod = formatOptions.dayPeriod;
    internalProps.hour = formatOptions.hour;
    internalProps.minute = formatOptions.minute;
    internalProps.second = formatOptions.second;
    internalProps.fractionalSecondDigits = formatOptions.fractionalSecondDigits;
    internalProps.timeZoneName = formatOptions.timeZoneName;
  }

  // The caller is responsible for associating |internalProps| with the right
  // object using |setInternalProperties|.
  return internalProps;
}

/**
 * Returns an object containing the DateTimeFormat internal properties of |obj|.
 */
function getDateTimeFormatInternals(obj) {
  assert(IsObject(obj), "getDateTimeFormatInternals called with non-object");
  assert(
    intl_GuardToDateTimeFormat(obj) !== null,
    "getDateTimeFormatInternals called with non-DateTimeFormat"
  );

  var internals = getIntlObjectInternals(obj);
  assert(
    internals.type === "DateTimeFormat",
    "bad type escaped getIntlObjectInternals"
  );

  // If internal properties have already been computed, use them.
  var internalProps = maybeInternalProperties(internals);
  if (internalProps) {
    return internalProps;
  }

  // Otherwise it's time to fully create them.
  internalProps = resolveDateTimeFormatInternals(internals.lazyData);
  setInternalProperties(internals, internalProps);
  return internalProps;
}

/**
 * 12.1.10 UnwrapDateTimeFormat( dtf )
 */
function UnwrapDateTimeFormat(dtf) {
  // Steps 2 and 4 (error handling moved to caller).
  if (
    IsObject(dtf) &&
    intl_GuardToDateTimeFormat(dtf) === null &&
    !intl_IsWrappedDateTimeFormat(dtf) &&
    callFunction(
      std_Object_isPrototypeOf,
      GetBuiltinPrototype("DateTimeFormat"),
      dtf
    )
  ) {
    dtf = dtf[intlFallbackSymbol()];
  }
  return dtf;
}

/**
 * 6.4.2 CanonicalizeTimeZoneName ( timeZone )
 *
 * Canonicalizes the given IANA time zone name.
 *
 * ES2017 Intl draft rev 4a23f407336d382ed5e3471200c690c9b020b5f3
 */
function CanonicalizeTimeZoneName(timeZone) {
  assert(typeof timeZone === "string", "CanonicalizeTimeZoneName");

  // Step 1. (Not applicable, the input is already a valid IANA time zone.)
  assert(timeZone !== "Etc/Unknown", "Invalid time zone");
  assert(
    timeZone === intl_IsValidTimeZoneName(timeZone),
    "Time zone name not normalized"
  );

  // Step 2.
  var ianaTimeZone = intl_canonicalizeTimeZone(timeZone);
  assert(ianaTimeZone !== "Etc/Unknown", "Invalid canonical time zone");
  assert(
    ianaTimeZone === intl_IsValidTimeZoneName(ianaTimeZone),
    "Unsupported canonical time zone"
  );

  // Step 3. (Not applicable.)
  assert(ianaTimeZone !== "Etc/UTC", "Invalid link to UTC");
  assert(ianaTimeZone !== "Etc/GMT", "Invalid link to UTC");
  assert(ianaTimeZone !== "GMT", "Invalid link to UTC");

  // Step 4.
  return ianaTimeZone;
}

var timeZoneCache = {
  icuDefaultTimeZone: undefined,
  defaultTimeZone: undefined,
};

/**
 * 6.4.3 DefaultTimeZone ()
 *
 * Returns the IANA time zone name for the host environment's current time zone.
 *
 * ES2017 Intl draft rev 4a23f407336d382ed5e3471200c690c9b020b5f3
 */
function DefaultTimeZone() {
  if (intl_isDefaultTimeZone(timeZoneCache.icuDefaultTimeZone)) {
    return timeZoneCache.defaultTimeZone;
  }

  // Verify that the current ICU time zone is a valid ECMA-402 time zone.
  var icuDefaultTimeZone = intl_defaultTimeZone();
  var timeZone = intl_IsValidTimeZoneName(icuDefaultTimeZone);
  if (timeZone === null) {
    // Before defaulting to "UTC", try to represent the default time zone
    // using the Etc/GMT + offset format. This format only accepts full
    // hour offsets.
    var msPerHour = 60 * 60 * 1000;
    var offset = intl_defaultTimeZoneOffset();
    assert(
      offset === (offset | 0),
      "milliseconds offset shouldn't be able to exceed int32_t range"
    );
    var offsetHours = offset / msPerHour;
    var offsetHoursFraction = offset % msPerHour;
    if (offsetHoursFraction === 0) {
      // Etc/GMT + offset uses POSIX-style signs, i.e. a positive offset
      // means a location west of GMT.
      timeZone =
        "Etc/GMT" + (offsetHours < 0 ? "+" : "-") + std_Math_abs(offsetHours);

      // Check if the fallback is valid.
      timeZone = intl_IsValidTimeZoneName(timeZone);
    }

    // Fallback to "UTC" if everything else fails.
    if (timeZone === null) {
      timeZone = "UTC";
    }
  }

  // Canonicalize the ICU time zone, e.g. change Etc/UTC to UTC.
  var defaultTimeZone = CanonicalizeTimeZoneName(timeZone);

  timeZoneCache.defaultTimeZone = defaultTimeZone;
  timeZoneCache.icuDefaultTimeZone = icuDefaultTimeZone;

  return defaultTimeZone;
}

/**
 * 21.4.1.33.1 IsTimeZoneOffsetString ( offsetString )
 * 21.4.1.33.2 ParseTimeZoneOffsetString ( offsetString )
 * 11.1.3 FormatOffsetTimeZoneIdentifier ( offsetMinutes )
 *
 * Function to parse, validate, and normalize time zone offset strings.
 *
 * ES2024 draft rev 10d44bfce4640894a0ed366bb769f2700cc8839a
 * ES2024 Intl draft rev 2f002b2000bf8b908efb793767bcfd23620e06db
 */
function TimeZoneOffsetString(offsetString) {
  assert(typeof(offsetString) === "string", "offsetString is a string");

  // UTCOffset :::
  //   ASCIISign Hour
  //   ASCIISign Hour HourSubcomponents[+Extended]
  //   ASCIISign Hour HourSubcomponents[~Extended]
  //
  // ASCIISign ::: one of
  //   + -
  //
  // Hour :::
  //   0 DecimalDigit
  //   1 DecimalDigit
  //   20
  //   21
  //   22
  //   23
  //
  // HourSubcomponents[Extended] :::
  //   TimeSeparator[?Extended] MinuteSecond
  //
  // TimeSeparator[Extended] :::
  //   [+Extended] :
  //   [~Extended] [empty]
  //
  // MinuteSecond :::
  //   0 DecimalDigit
  //   1 DecimalDigit
  //   2 DecimalDigit
  //   3 DecimalDigit
  //   4 DecimalDigit
  //   5 DecimalDigit

  // Return if there are too few or too many characters for an offset string.
  if (offsetString.length < 3 || offsetString.length > 6) {
    return null;
  }

  // The first character must match |ASCIISign|.
  var sign = offsetString[0];
  if (sign !== "+" && sign !== "-") {
    return null;
  }

  // Read the next two characters for the |Hour| grammar production.
  var hourTens = offsetString[1];
  var hourOnes = offsetString[2];

  // Read the remaining characters for the optional |MinuteSecond| grammar production.
  var minutesTens = "0";
  var minutesOnes = "0";
  if (offsetString.length > 3) {
    // |TimeSeparator| is optional.
    var separatorLength = offsetString[3] === ":" ? 1 : 0;

    // Return if there are too many characters for an offset string.
    if (offsetString.length !== (5 + separatorLength)) {
      return null;
    }

    minutesTens = offsetString[3 + separatorLength];
    minutesOnes = offsetString[4 + separatorLength];
  }

  // Validate the characters match the |Hour| and |MinuteSecond| productions:
  // - hours must be in the range 0..23
  // - minutes must in the range 0..59
  if (
    hourTens < "0" ||
    hourOnes < "0" ||
    minutesTens < "0" ||
    minutesOnes < "0" ||
    hourTens > "2" ||
    hourOnes > "9" ||
    minutesTens > "5" ||
    minutesOnes > "9" ||
    (hourTens === "2" && hourOnes > "3")
  ) {
    return null;
  }

  // FormatOffsetTimeZoneIdentifier, steps 1-5.
  if (
    hourTens === "0" &&
    hourOnes === "0" &&
    minutesTens === "0" &&
    minutesOnes === "0"
  ) {
    sign = "+";
  }

  return sign + hourTens + hourOnes + ":" + minutesTens + minutesOnes;
}

/* eslint-disable complexity */
/**
 * 11.1.2 CreateDateTimeFormat ( newTarget, locales, options, required, defaults [ , toLocaleStringTimeZone ] )
 *
 * Initializes an object as a DateTimeFormat.
 *
 * This method is complicated a moderate bit by its implementing initialization
 * as a *lazy* concept.  Everything that must happen now, does -- but we defer
 * all the work we can until the object is actually used as a DateTimeFormat.
 * This later work occurs in |resolveDateTimeFormatInternals|; steps not noted
 * here occur there.
 */
function InitializeDateTimeFormat(
  dateTimeFormat,
  thisValue,
  locales,
  options,
  required,
  defaults,
  toLocaleStringTimeZone,
  mozExtensions
) {
  assert(
    IsObject(dateTimeFormat),
    "InitializeDateTimeFormat called with non-Object"
  );
  assert(
    intl_GuardToDateTimeFormat(dateTimeFormat) !== null,
    "InitializeDateTimeFormat called with non-DateTimeFormat"
  );
  assert(
    required === "date" || required === "time" || required === "any",
    `InitializeDateTimeFormat called with invalid required value: ${required}`
  );
  assert(
    defaults === "date" || defaults === "time" || defaults === "all",
    `InitializeDateTimeFormat called with invalid defaults value: ${defaults}`
  );
  assert(
    toLocaleStringTimeZone === undefined || typeof toLocaleStringTimeZone === "string",
    `InitializeDateTimeFormat called with invalid toLocaleStringTimeZone value: ${toLocaleStringTimeZone}`
  );

  // Lazy DateTimeFormat data has the following structure:
  //
  //   {
  //     requestedLocales: List of locales,
  //
  //     localeOpt: // *first* opt computed in InitializeDateTimeFormat
  //       {
  //         localeMatcher: "lookup" / "best fit",
  //
  //         ca: string matching a Unicode extension type, // optional
  //
  //         nu: string matching a Unicode extension type, // optional
  //
  //         hc: "h11" / "h12" / "h23" / "h24", // optional
  //       }
  //
  //     timeZone: IANA time zone name or a normalized time zone offset string,
  //
  //     formatOptions: // *second* opt computed in InitializeDateTimeFormat
  //       {
  //         // all the properties/values listed in Table 3
  //         // (weekday, era, year, month, day, &c.)
  //
  //         hour12: true / false,  // optional
  //       }
  //
  //     formatMatcher: "basic" / "best fit",
  //
  //     required: "date" / "time" / "any", // optional
  //
  //     defaults: "date" / "time" / "all", // optional
  //   }
  //
  // Note that lazy data is only installed as a final step of initialization,
  // so every DateTimeFormat lazy data object has *all* these properties,
  // never a subset of them.
  var lazyDateTimeFormatData = std_Object_create(null);

  // Step 1. (Performed in caller)

  // Step 2.
  var requestedLocales = CanonicalizeLocaleList(locales);
  lazyDateTimeFormatData.requestedLocales = requestedLocales;

  // Step 3. (Inlined call to CoerceOptionsToObject.)
  if (options === undefined) {
    options = std_Object_create(null);
  } else {
    options = ToObject(options);
  }

  // Compute options that impact interpretation of locale.
  // Step 4.
  var localeOpt = new_Record();
  lazyDateTimeFormatData.localeOpt = localeOpt;

  // Steps 5-6.
  var localeMatcher = GetOption(
    options,
    "localeMatcher",
    "string",
    ["lookup", "best fit"],
    "best fit"
  );
  localeOpt.localeMatcher = localeMatcher;

  // Step 7.
  var calendar = GetOption(options, "calendar", "string", undefined, undefined);

  // Step 8.
  if (calendar !== undefined) {
    calendar = intl_ValidateAndCanonicalizeUnicodeExtensionType(
      calendar,
      "calendar",
      "ca"
    );
  }

  // Step 9.
  localeOpt.ca = calendar;

  // Step 10.
  var numberingSystem = GetOption(
    options,
    "numberingSystem",
    "string",
    undefined,
    undefined
  );

  // Step 11.
  if (numberingSystem !== undefined) {
    numberingSystem = intl_ValidateAndCanonicalizeUnicodeExtensionType(
      numberingSystem,
      "numberingSystem",
      "nu"
    );
  }

  // Step 12.
  localeOpt.nu = numberingSystem;

  // Step 13.
  var hour12 = GetOption(options, "hour12", "boolean", undefined, undefined);

  // Step 14.
  var hourCycle = GetOption(
    options,
    "hourCycle",
    "string",
    ["h11", "h12", "h23", "h24"],
    undefined
  );

  // Step 15.
  if (hour12 !== undefined) {
    // The "hourCycle" option is ignored if "hr12" is also present.
    hourCycle = null;
  }

  // Step 16.
  localeOpt.hc = hourCycle;

  // Steps 17-29 (see resolveDateTimeFormatInternals).

  // Step 29.
  var timeZone = options.timeZone;

  // Steps 30-34.
  if (timeZone === undefined) {
    // Step 30.a.
    if (toLocaleStringTimeZone !== undefined) {
      timeZone = toLocaleStringTimeZone;
    } else {
      timeZone = DefaultTimeZone();
    }

    // Steps 32-34. (Not applicable in our implementation.)
  } else {
    // Step 31.a.
    if (toLocaleStringTimeZone !== undefined) {
      ThrowTypeError(
        JSMSG_INVALID_DATETIME_OPTION,
        "timeZone",
        "Temporal.ZonedDateTime.toLocaleString"
      );
    }
    timeZone = ToString(timeZone);

    // Steps 32-34.
    var offsetString = TimeZoneOffsetString(timeZone);
    if (offsetString !== null) {
      // Steps 32.a-g. (Performed in TimeZoneOffsetString in our implementation.)
      timeZone = offsetString;
    } else {
      // Steps 33-34.
      var validTimeZone = intl_IsValidTimeZoneName(timeZone);
      if (validTimeZone !== null) {
        // Step 33.a.
        timeZone = CanonicalizeTimeZoneName(validTimeZone);
      } else {
        // Step 34.a.
        ThrowRangeError(JSMSG_INVALID_TIME_ZONE, timeZone);
      }
    }
  }

  // Step 33.
  lazyDateTimeFormatData.timeZone = timeZone;

  // Step 34.
  var formatOptions = new_Record();
  lazyDateTimeFormatData.formatOptions = formatOptions;

  if (mozExtensions) {
    var pattern = GetOption(options, "pattern", "string", undefined, undefined);
    lazyDateTimeFormatData.patternOption = pattern;
  }

  // Step 35.
  //
  // Pass hr12 on to ICU. The hour cycle option is passed through |localeOpt|.
  if (hour12 !== undefined) {
    formatOptions.hour12 = hour12;
  }

  // Step 36. (Explicit format component computed in step 43.)

  // Step 37.
  // 11.5, Table 7: Components of date and time formats.
  formatOptions.weekday = GetOption(
    options,
    "weekday",
    "string",
    ["narrow", "short", "long"],
    undefined
  );
  formatOptions.era = GetOption(
    options,
    "era",
    "string",
    ["narrow", "short", "long"],
    undefined
  );
  formatOptions.year = GetOption(
    options,
    "year",
    "string",
    ["2-digit", "numeric"],
    undefined
  );
  formatOptions.month = GetOption(
    options,
    "month",
    "string",
    ["2-digit", "numeric", "narrow", "short", "long"],
    undefined
  );
  formatOptions.day = GetOption(
    options,
    "day",
    "string",
    ["2-digit", "numeric"],
    undefined
  );
  formatOptions.dayPeriod = GetOption(
    options,
    "dayPeriod",
    "string",
    ["narrow", "short", "long"],
    undefined
  );
  formatOptions.hour = GetOption(
    options,
    "hour",
    "string",
    ["2-digit", "numeric"],
    undefined
  );
  formatOptions.minute = GetOption(
    options,
    "minute",
    "string",
    ["2-digit", "numeric"],
    undefined
  );
  formatOptions.second = GetOption(
    options,
    "second",
    "string",
    ["2-digit", "numeric"],
    undefined
  );
  formatOptions.fractionalSecondDigits = GetNumberOption(
    options,
    "fractionalSecondDigits",
    1,
    3,
    undefined
  );
  formatOptions.timeZoneName = GetOption(
    options,
    "timeZoneName",
    "string",
    [
      "short",
      "long",
      "shortOffset",
      "longOffset",
      "shortGeneric",
      "longGeneric",
    ],
    undefined
  );

  // Step 38.
  //
  // For some reason (ICU not exposing enough interface?) we drop the
  // requested format matcher on the floor after this.  In any case, even if
  // doing so is justified, we have to do this work here in case it triggers
  // getters or similar. (bug 852837)
  var formatMatcher = GetOption(
    options,
    "formatMatcher",
    "string",
    ["basic", "best fit"],
    "best fit"
  );
  void formatMatcher;

  // Steps 39-40.
  var dateStyle = GetOption(
    options,
    "dateStyle",
    "string",
    ["full", "long", "medium", "short"],
    undefined
  );
  lazyDateTimeFormatData.dateStyle = dateStyle;

  // Steps 41-42.
  var timeStyle = GetOption(
    options,
    "timeStyle",
    "string",
    ["full", "long", "medium", "short"],
    undefined
  );
  lazyDateTimeFormatData.timeStyle = timeStyle;

  // Step 43.
  if (dateStyle !== undefined || timeStyle !== undefined) {
    /* eslint-disable no-nested-ternary */
    var explicitFormatComponent =
      formatOptions.weekday !== undefined
        ? "weekday"
        : formatOptions.era !== undefined
        ? "era"
        : formatOptions.year !== undefined
        ? "year"
        : formatOptions.month !== undefined
        ? "month"
        : formatOptions.day !== undefined
        ? "day"
        : formatOptions.dayPeriod !== undefined
        ? "dayPeriod"
        : formatOptions.hour !== undefined
        ? "hour"
        : formatOptions.minute !== undefined
        ? "minute"
        : formatOptions.second !== undefined
        ? "second"
        : formatOptions.fractionalSecondDigits !== undefined
        ? "fractionalSecondDigits"
        : formatOptions.timeZoneName !== undefined
        ? "timeZoneName"
        : undefined;
    /* eslint-enable no-nested-ternary */

    // Step 43.a.
    if (explicitFormatComponent !== undefined) {
      ThrowTypeError(
        JSMSG_INVALID_DATETIME_OPTION,
        explicitFormatComponent,
        dateStyle !== undefined ? "dateStyle" : "timeStyle"
      );
    }

    // Step 43.b.
    if (required === "date" && timeStyle !== undefined) {
      ThrowTypeError(JSMSG_INVALID_DATETIME_STYLE, "timeStyle", "date");
    }

    // Step 43.c.
    if (required === "time" && dateStyle !== undefined) {
      ThrowTypeError(JSMSG_INVALID_DATETIME_STYLE, "dateStyle", "time");
    }
  } else {
    lazyDateTimeFormatData.required = required;
    lazyDateTimeFormatData.defaults = defaults;

    // Steps 44.f-h provided by ICU, more or less.
  }

  // Steps 45-50. (see resolveDateTimeFormatInternals).

  // We've done everything that must be done now: mark the lazy data as fully
  // computed and install it.
  initializeIntlObject(
    dateTimeFormat,
    "DateTimeFormat",
    lazyDateTimeFormatData
  );

  // 11.1.1 Intl.DateTimeFormat, step 3. (Inlined call to ChainDateTimeFormat.)
  if (
    dateTimeFormat !== thisValue &&
    callFunction(
      std_Object_isPrototypeOf,
      GetBuiltinPrototype("DateTimeFormat"),
      thisValue
    )
  ) {
    DefineDataProperty(
      thisValue,
      intlFallbackSymbol(),
      dateTimeFormat,
      ATTR_NONENUMERABLE | ATTR_NONCONFIGURABLE | ATTR_NONWRITABLE
    );

    return thisValue;
  }

  // Step 51.
  return dateTimeFormat;
}
/* eslint-enable complexity */

/**
 * Returns the subset of the given locale list for which this locale list has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.3.2.
 */
function Intl_DateTimeFormat_supportedLocalesOf(locales /*, options*/) {
  var options = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  // Step 1.
  var availableLocales = "DateTimeFormat";

  // Step 2.
  var requestedLocales = CanonicalizeLocaleList(locales);

  // Step 3.
  return SupportedLocales(availableLocales, requestedLocales, options);
}

/**
 * DateTimeFormat internal properties.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.1 and 12.3.3.
 */
var dateTimeFormatInternalProperties = {
  localeData: dateTimeFormatLocaleData,
  relevantExtensionKeys: ["ca", "hc", "nu"],
};

function dateTimeFormatLocaleData() {
  return {
    ca: intl_availableCalendars,
    nu: getNumberingSystems,
    hc: () => {
      return [null, "h11", "h12", "h23", "h24"];
    },
    default: {
      ca: intl_defaultCalendar,
      nu: intl_numberingSystem,
      hc: () => {
        return null;
      },
    },
  };
}

/**
 * Create function to be cached and returned by Intl.DateTimeFormat.prototype.format.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.1.5.
 */
function createDateTimeFormatFormat(dtf) {
  // This function is not inlined in $Intl_DateTimeFormat_format_get to avoid
  // creating a call-object on each call to $Intl_DateTimeFormat_format_get.
  return function(date) {
    // Step 1 (implicit).

    // Step 2.
    assert(IsObject(dtf), "dateTimeFormatFormatToBind called with non-Object");
    assert(
      intl_GuardToDateTimeFormat(dtf) !== null,
      "dateTimeFormatFormatToBind called with non-DateTimeFormat"
    );

    // Steps 3-5.
    return intl_FormatDateTime(dtf, date, /* formatToParts = */ false);
  };
}

/**
 * Returns a function bound to this DateTimeFormat that returns a String value
 * representing the result of calling ToNumber(date) according to the
 * effective locale and the formatting options of this DateTimeFormat.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.4.3.
 */
// Uncloned functions with `$` prefix are allocated as extended function
// to store the original name in `SetCanonicalName`.
function $Intl_DateTimeFormat_format_get() {
  // Steps 1-3.
  var thisArg = UnwrapDateTimeFormat(this);
  var dtf = thisArg;
  if (!IsObject(dtf) || (dtf = intl_GuardToDateTimeFormat(dtf)) === null) {
    return callFunction(
      intl_CallDateTimeFormatMethodIfWrapped,
      thisArg,
      "$Intl_DateTimeFormat_format_get"
    );
  }

  var internals = getDateTimeFormatInternals(dtf);

  // Step 4.
  if (internals.boundFormat === undefined) {
    // Steps 4.a-c.
    internals.boundFormat = createDateTimeFormatFormat(dtf);
  }

  // Step 5.
  return internals.boundFormat;
}
SetCanonicalName($Intl_DateTimeFormat_format_get, "get format");

/**
 * Intl.DateTimeFormat.prototype.formatToParts ( date )
 *
 * Spec: ECMAScript Internationalization API Specification, 12.4.4.
 */
function Intl_DateTimeFormat_formatToParts(date) {
  // Step 1.
  var dtf = this;

  // Steps 2-3.
  if (!IsObject(dtf) || (dtf = intl_GuardToDateTimeFormat(dtf)) === null) {
    return callFunction(
      intl_CallDateTimeFormatMethodIfWrapped,
      this,
      date,
      "Intl_DateTimeFormat_formatToParts"
    );
  }

  // Ensure the DateTimeFormat internals are resolved.
  getDateTimeFormatInternals(dtf);

  // Steps 4-6.
  return intl_FormatDateTime(dtf, date, /* formatToParts = */ true);
}

/**
 * Intl.DateTimeFormat.prototype.formatRange ( startDate , endDate )
 *
 * Spec: Intl.DateTimeFormat.prototype.formatRange proposal
 */
function Intl_DateTimeFormat_formatRange(startDate, endDate) {
  // Step 1.
  var dtf = this;

  // Step 2.
  if (!IsObject(dtf) || (dtf = intl_GuardToDateTimeFormat(dtf)) === null) {
    return callFunction(
      intl_CallDateTimeFormatMethodIfWrapped,
      this,
      startDate,
      endDate,
      "Intl_DateTimeFormat_formatRange"
    );
  }

  // Step 3.
  if (startDate === undefined || endDate === undefined) {
    ThrowTypeError(
      JSMSG_UNDEFINED_DATE,
      startDate === undefined ? "start" : "end",
      "formatRange"
    );
  }

  // Ensure the DateTimeFormat internals are resolved.
  getDateTimeFormatInternals(dtf);

  // Steps 4-6.
  return intl_FormatDateTimeRange(dtf, startDate, endDate, /* formatToParts = */ false);
}

/**
 * Intl.DateTimeFormat.prototype.formatRangeToParts ( startDate , endDate )
 *
 * Spec: Intl.DateTimeFormat.prototype.formatRange proposal
 */
function Intl_DateTimeFormat_formatRangeToParts(startDate, endDate) {
  // Step 1.
  var dtf = this;

  // Step 2.
  if (!IsObject(dtf) || (dtf = intl_GuardToDateTimeFormat(dtf)) === null) {
    return callFunction(
      intl_CallDateTimeFormatMethodIfWrapped,
      this,
      startDate,
      endDate,
      "Intl_DateTimeFormat_formatRangeToParts"
    );
  }

  // Step 3.
  if (startDate === undefined || endDate === undefined) {
    ThrowTypeError(
      JSMSG_UNDEFINED_DATE,
      startDate === undefined ? "start" : "end",
      "formatRangeToParts"
    );
  }

  // Ensure the DateTimeFormat internals are resolved.
  getDateTimeFormatInternals(dtf);

  // Steps 4-6.
  return intl_FormatDateTimeRange(dtf, startDate, endDate, /* formatToParts = */ true);
}

/**
 * Returns the resolved options for a DateTimeFormat object.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.4.5.
 */
function Intl_DateTimeFormat_resolvedOptions() {
  // Steps 1-3.
  var thisArg = UnwrapDateTimeFormat(this);
  var dtf = thisArg;
  if (!IsObject(dtf) || (dtf = intl_GuardToDateTimeFormat(dtf)) === null) {
    return callFunction(
      intl_CallDateTimeFormatMethodIfWrapped,
      thisArg,
      "Intl_DateTimeFormat_resolvedOptions"
    );
  }

  // Ensure the internals are resolved.
  var internals = getDateTimeFormatInternals(dtf);

  // Steps 4-5.
  var result = {
    locale: internals.locale,
    calendar: internals.calendar,
    numberingSystem: internals.numberingSystem,
    timeZone: internals.timeZone,
  };

  if (internals.pattern !== undefined) {
    // The raw pattern option is only internal to Mozilla, and not part of the
    // ECMA-402 API.
    DefineDataProperty(result, "pattern", internals.pattern);
  }

  var hasDateStyle = internals.dateStyle !== undefined;
  var hasTimeStyle = internals.timeStyle !== undefined;

  if (hasDateStyle || hasTimeStyle) {
    if (hasTimeStyle) {
      // timeStyle (unlike dateStyle) requires resolving the pattern to
      // ensure "hourCycle" and "hour12" properties are added to |result|.
      intl_resolveDateTimeFormatComponents(
        dtf,
        result,
        /* includeDateTimeFields = */ false
      );
    }
    if (hasDateStyle) {
      DefineDataProperty(result, "dateStyle", internals.dateStyle);
    }
    if (hasTimeStyle) {
      DefineDataProperty(result, "timeStyle", internals.timeStyle);
    }
  } else {
    // Components bag or a (Mozilla-only) raw pattern.
    intl_resolveDateTimeFormatComponents(
      dtf,
      result,
      /* includeDateTimeFields = */ true
    );
  }

  // Step 6.
  return result;
}
