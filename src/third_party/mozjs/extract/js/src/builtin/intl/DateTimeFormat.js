/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Portions Copyright Norbert Lindenberg 2011-2012. */

/**
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
  //     timeZone: IANA time zone name,
  //
  //     formatOpt: // *second* opt computed in InitializeDateTimeFormat
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

  // Step 10.
  var localeData = DateTimeFormat.localeData;

  // Step 11.
  var r = ResolveLocale(
    "DateTimeFormat",
    lazyDateTimeFormatData.requestedLocales,
    lazyDateTimeFormatData.localeOpt,
    DateTimeFormat.relevantExtensionKeys,
    localeData
  );

  // Steps 12-13, 15.
  internalProps.locale = r.locale;
  internalProps.calendar = r.ca;
  internalProps.numberingSystem = r.nu;

  // Step 20.
  internalProps.timeZone = lazyDateTimeFormatData.timeZone;

  // Step 21.
  var formatOpt = lazyDateTimeFormatData.formatOpt;

  // Step 14.
  // Copy the hourCycle setting, if present, to the format options. But
  // only do this if no hour12 option is present, because the latter takes
  // precedence over hourCycle.
  if (r.hc !== null && formatOpt.hour12 === undefined) {
    formatOpt.hourCycle = r.hc;
  }

  // Steps 26-31, more or less - see comment after this function.
  if (lazyDateTimeFormatData.patternOption !== undefined) {
    internalProps.pattern = lazyDateTimeFormatData.patternOption;
  } else if (
    lazyDateTimeFormatData.dateStyle !== undefined ||
    lazyDateTimeFormatData.timeStyle !== undefined
  ) {
    internalProps.hourCycle = formatOpt.hourCycle;
    internalProps.hour12 = formatOpt.hour12;
    internalProps.dateStyle = lazyDateTimeFormatData.dateStyle;
    internalProps.timeStyle = lazyDateTimeFormatData.timeStyle;
  } else {
    internalProps.hourCycle = formatOpt.hourCycle;
    internalProps.hour12 = formatOpt.hour12;
    internalProps.weekday = formatOpt.weekday;
    internalProps.era = formatOpt.era;
    internalProps.year = formatOpt.year;
    internalProps.month = formatOpt.month;
    internalProps.day = formatOpt.day;
    internalProps.dayPeriod = formatOpt.dayPeriod;
    internalProps.hour = formatOpt.hour;
    internalProps.minute = formatOpt.minute;
    internalProps.second = formatOpt.second;
    internalProps.fractionalSecondDigits = formatOpt.fractionalSecondDigits;
    internalProps.timeZoneName = formatOpt.timeZoneName;
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

  // Step 3.
  if (ianaTimeZone === "Etc/UTC" || ianaTimeZone === "Etc/GMT") {
    ianaTimeZone = "UTC";
  }

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
    const msPerHour = 60 * 60 * 1000;
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
 * Initializes an object as a DateTimeFormat.
 *
 * This method is complicated a moderate bit by its implementing initialization
 * as a *lazy* concept.  Everything that must happen now, does -- but we defer
 * all the work we can until the object is actually used as a DateTimeFormat.
 * This later work occurs in |resolveDateTimeFormatInternals|; steps not noted
 * here occur there.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.1.1.
 */
function InitializeDateTimeFormat(
  dateTimeFormat,
  thisValue,
  locales,
  options,
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
  //     timeZone: IANA time zone name,
  //
  //     formatOpt: // *second* opt computed in InitializeDateTimeFormat
  //       {
  //         // all the properties/values listed in Table 3
  //         // (weekday, era, year, month, day, &c.)
  //
  //         hour12: true / false,  // optional
  //       }
  //
  //     formatMatcher: "basic" / "best fit",
  //   }
  //
  // Note that lazy data is only installed as a final step of initialization,
  // so every DateTimeFormat lazy data object has *all* these properties,
  // never a subset of them.
  var lazyDateTimeFormatData = std_Object_create(null);

  // Step 1.
  var requestedLocales = CanonicalizeLocaleList(locales);
  lazyDateTimeFormatData.requestedLocales = requestedLocales;

  // Step 2.
  options = ToDateTimeOptions(options, "any", "date");

  // Compute options that impact interpretation of locale.
  // Step 3.
  var localeOpt = new_Record();
  lazyDateTimeFormatData.localeOpt = localeOpt;

  // Steps 4-5.
  var localeMatcher = GetOption(
    options,
    "localeMatcher",
    "string",
    ["lookup", "best fit"],
    "best fit"
  );
  localeOpt.localeMatcher = localeMatcher;

  var calendar = GetOption(options, "calendar", "string", undefined, undefined);

  if (calendar !== undefined) {
    calendar = intl_ValidateAndCanonicalizeUnicodeExtensionType(
      calendar,
      "calendar",
      "ca"
    );
  }

  localeOpt.ca = calendar;

  var numberingSystem = GetOption(
    options,
    "numberingSystem",
    "string",
    undefined,
    undefined
  );

  if (numberingSystem !== undefined) {
    numberingSystem = intl_ValidateAndCanonicalizeUnicodeExtensionType(
      numberingSystem,
      "numberingSystem",
      "nu"
    );
  }

  localeOpt.nu = numberingSystem;

  // Step 6.
  var hr12 = GetOption(options, "hour12", "boolean", undefined, undefined);

  // Step 7.
  var hc = GetOption(
    options,
    "hourCycle",
    "string",
    ["h11", "h12", "h23", "h24"],
    undefined
  );

  // Step 8.
  if (hr12 !== undefined) {
    // The "hourCycle" option is ignored if "hr12" is also present.
    hc = null;
  }

  // Step 9.
  localeOpt.hc = hc;

  // Steps 10-16 (see resolveDateTimeFormatInternals).

  // Steps 17-20.
  var tz = options.timeZone;
  if (tz !== undefined) {
    // Step 18.a.
    tz = ToString(tz);

    // Step 18.b.
    var timeZone = intl_IsValidTimeZoneName(tz);
    if (timeZone === null) {
      ThrowRangeError(JSMSG_INVALID_TIME_ZONE, tz);
    }

    // Step 18.c.
    tz = CanonicalizeTimeZoneName(timeZone);
  } else {
    // Step 19.
    tz = DefaultTimeZone();
  }
  lazyDateTimeFormatData.timeZone = tz;

  // Step 21.
  var formatOpt = new_Record();
  lazyDateTimeFormatData.formatOpt = formatOpt;

  if (mozExtensions) {
    let pattern = GetOption(options, "pattern", "string", undefined, undefined);
    lazyDateTimeFormatData.patternOption = pattern;
  }

  // Step 22.
  // 12.1, Table 5: Components of date and time formats.
  formatOpt.weekday = GetOption(
    options,
    "weekday",
    "string",
    ["narrow", "short", "long"],
    undefined
  );
  formatOpt.era = GetOption(
    options,
    "era",
    "string",
    ["narrow", "short", "long"],
    undefined
  );
  formatOpt.year = GetOption(
    options,
    "year",
    "string",
    ["2-digit", "numeric"],
    undefined
  );
  formatOpt.month = GetOption(
    options,
    "month",
    "string",
    ["2-digit", "numeric", "narrow", "short", "long"],
    undefined
  );
  formatOpt.day = GetOption(
    options,
    "day",
    "string",
    ["2-digit", "numeric"],
    undefined
  );
  formatOpt.dayPeriod = GetOption(
    options,
    "dayPeriod",
    "string",
    ["narrow", "short", "long"],
    undefined
  );
  formatOpt.hour = GetOption(
    options,
    "hour",
    "string",
    ["2-digit", "numeric"],
    undefined
  );
  formatOpt.minute = GetOption(
    options,
    "minute",
    "string",
    ["2-digit", "numeric"],
    undefined
  );
  formatOpt.second = GetOption(
    options,
    "second",
    "string",
    ["2-digit", "numeric"],
    undefined
  );
  formatOpt.fractionalSecondDigits = GetNumberOption(
    options,
    "fractionalSecondDigits",
    1,
    3,
    undefined
  );
  formatOpt.timeZoneName = GetOption(
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

  // Steps 23-24 provided by ICU - see comment after this function.

  // Step 25.
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

  // "DateTimeFormat dateStyle & timeStyle" propsal
  // https://github.com/tc39/proposal-intl-datetime-style
  var dateStyle = GetOption(
    options,
    "dateStyle",
    "string",
    ["full", "long", "medium", "short"],
    undefined
  );
  lazyDateTimeFormatData.dateStyle = dateStyle;

  var timeStyle = GetOption(
    options,
    "timeStyle",
    "string",
    ["full", "long", "medium", "short"],
    undefined
  );
  lazyDateTimeFormatData.timeStyle = timeStyle;

  if (dateStyle !== undefined || timeStyle !== undefined) {
    var optionsList = [
      "weekday",
      "era",
      "year",
      "month",
      "day",
      "dayPeriod",
      "hour",
      "minute",
      "second",
      "fractionalSecondDigits",
      "timeZoneName",
    ];

    for (var i = 0; i < optionsList.length; i++) {
      var option = optionsList[i];
      if (formatOpt[option] !== undefined) {
        ThrowTypeError(
          JSMSG_INVALID_DATETIME_OPTION,
          option,
          dateStyle !== undefined ? "dateStyle" : "timeStyle"
        );
      }
    }
  }

  // Steps 26-28 provided by ICU, more or less - see comment after this function.

  // Steps 29-30.
  // Pass hr12 on to ICU.
  if (hr12 !== undefined) {
    formatOpt.hour12 = hr12;
  }

  // Step 32.
  //
  // We've done everything that must be done now: mark the lazy data as fully
  // computed and install it.
  initializeIntlObject(
    dateTimeFormat,
    "DateTimeFormat",
    lazyDateTimeFormatData
  );

  // 12.2.1, steps 4-5.
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

  // 12.2.1, step 6.
  return dateTimeFormat;
}

/**
 * Returns a new options object that includes the provided options (if any)
 * and fills in default components if required components are not defined.
 * Required can be "date", "time", or "any".
 * Defaults can be "date", "time", or "all".
 *
 * Spec: ECMAScript Internationalization API Specification, 12.1.1.
 */
function ToDateTimeOptions(options, required, defaults) {
  assert(typeof required === "string", "ToDateTimeOptions");
  assert(typeof defaults === "string", "ToDateTimeOptions");

  // Steps 1-2.
  if (options === undefined) {
    options = null;
  } else {
    options = ToObject(options);
  }
  options = std_Object_create(options);

  // Step 3.
  var needDefaults = true;

  // Step 4.
  if (required === "date" || required === "any") {
    if (options.weekday !== undefined) {
      needDefaults = false;
    }
    if (options.year !== undefined) {
      needDefaults = false;
    }
    if (options.month !== undefined) {
      needDefaults = false;
    }
    if (options.day !== undefined) {
      needDefaults = false;
    }
  }

  // Step 5.
  if (required === "time" || required === "any") {
    if (options.dayPeriod !== undefined) {
      needDefaults = false;
    }
    if (options.hour !== undefined) {
      needDefaults = false;
    }
    if (options.minute !== undefined) {
      needDefaults = false;
    }
    if (options.second !== undefined) {
      needDefaults = false;
    }
    if (options.fractionalSecondDigits !== undefined) {
      needDefaults = false;
    }
  }

  // "DateTimeFormat dateStyle & timeStyle" propsal
  // https://github.com/tc39/proposal-intl-datetime-style
  var dateStyle = options.dateStyle;
  var timeStyle = options.timeStyle;

  if (dateStyle !== undefined || timeStyle !== undefined) {
    needDefaults = false;
  }

  if (required === "date" && timeStyle !== undefined) {
    ThrowTypeError(
      JSMSG_INVALID_DATETIME_STYLE,
      "timeStyle",
      "toLocaleDateString"
    );
  }

  if (required === "time" && dateStyle !== undefined) {
    ThrowTypeError(
      JSMSG_INVALID_DATETIME_STYLE,
      "dateStyle",
      "toLocaleTimeString"
    );
  }

  // Step 6.
  if (needDefaults && (defaults === "date" || defaults === "all")) {
    // The specification says to call [[DefineOwnProperty]] with false for
    // the Throw parameter, while Object.defineProperty uses true. For the
    // calls here, the difference doesn't matter because we're adding
    // properties to a new object.
    DefineDataProperty(options, "year", "numeric");
    DefineDataProperty(options, "month", "numeric");
    DefineDataProperty(options, "day", "numeric");
  }

  // Step 7.
  if (needDefaults && (defaults === "time" || defaults === "all")) {
    // See comment for step 7.
    DefineDataProperty(options, "hour", "numeric");
    DefineDataProperty(options, "minute", "numeric");
    DefineDataProperty(options, "second", "numeric");
  }

  // Step 8.
  return options;
}

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

    // Steps 3-4.
    var x = date === undefined ? std_Date_now() : ToNumber(date);

    // Step 5.
    return intl_FormatDateTime(dtf, x, /* formatToParts = */ false);
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

  // Steps 4-5.
  var x = date === undefined ? std_Date_now() : ToNumber(date);

  // Ensure the DateTimeFormat internals are resolved.
  getDateTimeFormatInternals(dtf);

  // Step 6.
  return intl_FormatDateTime(dtf, x, /* formatToParts = */ true);
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

  // Step 4.
  var x = ToNumber(startDate);

  // Step 5.
  var y = ToNumber(endDate);

  // Ensure the DateTimeFormat internals are resolved.
  getDateTimeFormatInternals(dtf);

  // Step 6.
  return intl_FormatDateTimeRange(dtf, x, y, /* formatToParts = */ false);
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

  // Step 4.
  var x = ToNumber(startDate);

  // Step 5.
  var y = ToNumber(endDate);

  // Ensure the DateTimeFormat internals are resolved.
  getDateTimeFormatInternals(dtf);

  // Step 6.
  return intl_FormatDateTimeRange(dtf, x, y, /* formatToParts = */ true);
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
