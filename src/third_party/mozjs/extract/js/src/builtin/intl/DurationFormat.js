/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * DurationFormat internal properties.
 */
function durationFormatLocaleData() {
  return {
    nu: getNumberingSystems,
    default: {
      nu: intl_numberingSystem,
    },
  };
}
var durationFormatInternalProperties = {
  localeData: durationFormatLocaleData,
  relevantExtensionKeys: ["nu"],
};

/**
 * Intl.DurationFormat ( [ locales [ , options ] ] )
 *
 * Compute an internal properties object from |lazyDurationFormatData|.
 */
function resolveDurationFormatInternals(lazyDurationFormatData) {
  assert(IsObject(lazyDurationFormatData), "lazy data not an object?");

  var internalProps = std_Object_create(null);

  var DurationFormat = durationFormatInternalProperties;

  // Compute effective locale.

  // Step 9.
  var r = ResolveLocale(
    "DurationFormat",
    lazyDurationFormatData.requestedLocales,
    lazyDurationFormatData.opt,
    DurationFormat.relevantExtensionKeys,
    DurationFormat.localeData
  );

  // Steps 10-11.
  internalProps.locale = r.locale;

  // Steps 12-21. (Not applicable in our implementation.)

  // Step 22.
  internalProps.numberingSystem = r.nu;

  // Step 24.
  internalProps.style = lazyDurationFormatData.style;

  // Step 26.
  internalProps.yearsStyle = lazyDurationFormatData.yearsStyle;
  internalProps.yearsDisplay = lazyDurationFormatData.yearsDisplay;

  internalProps.weeksStyle = lazyDurationFormatData.weeksStyle;
  internalProps.weeksDisplay = lazyDurationFormatData.weeksDisplay;

  internalProps.monthsStyle = lazyDurationFormatData.monthsStyle;
  internalProps.monthsDisplay = lazyDurationFormatData.monthsDisplay;

  internalProps.daysStyle = lazyDurationFormatData.daysStyle;
  internalProps.daysDisplay = lazyDurationFormatData.daysDisplay;

  internalProps.hoursStyle = lazyDurationFormatData.hoursStyle;
  internalProps.hoursDisplay = lazyDurationFormatData.hoursDisplay;

  internalProps.minutesStyle = lazyDurationFormatData.minutesStyle;
  internalProps.minutesDisplay = lazyDurationFormatData.minutesDisplay;

  internalProps.secondsStyle = lazyDurationFormatData.secondsStyle;
  internalProps.secondsDisplay = lazyDurationFormatData.secondsDisplay;

  internalProps.millisecondsStyle = lazyDurationFormatData.millisecondsStyle;
  internalProps.millisecondsDisplay =
    lazyDurationFormatData.millisecondsDisplay;

  internalProps.microsecondsStyle = lazyDurationFormatData.microsecondsStyle;
  internalProps.microsecondsDisplay =
    lazyDurationFormatData.microsecondsDisplay;

  internalProps.nanosecondsStyle = lazyDurationFormatData.nanosecondsStyle;
  internalProps.nanosecondsDisplay = lazyDurationFormatData.nanosecondsDisplay;

  // Step 27.
  internalProps.fractionalDigits = lazyDurationFormatData.fractionalDigits;

  // The caller is responsible for associating |internalProps| with the right
  // object using |setInternalProperties|.
  return internalProps;
}

/**
 * Returns an object containing the DurationFormat internal properties of |obj|.
 */
function getDurationFormatInternals(obj) {
  assert(IsObject(obj), "getDurationFormatInternals called with non-object");
  assert(
    intl_GuardToDurationFormat(obj) !== null,
    "getDurationFormatInternals called with non-DurationFormat"
  );

  var internals = getIntlObjectInternals(obj);
  assert(
    internals.type === "DurationFormat",
    "bad type escaped getIntlObjectInternals"
  );

  // If internal properties have already been computed, use them.
  var internalProps = maybeInternalProperties(internals);
  if (internalProps) {
    return internalProps;
  }

  // Otherwise it's time to fully create them.
  internalProps = resolveDurationFormatInternals(internals.lazyData);
  setInternalProperties(internals, internalProps);
  return internalProps;
}

/**
 * Intl.DurationFormat ( [ locales [ , options ] ] )
 *
 * Initializes an object as a DurationFormat.
 *
 * This method is complicated a moderate bit by its implementing initialization
 * as a *lazy* concept.  Everything that must happen now, does -- but we defer
 * all the work we can until the object is actually used as a DurationFormat.
 * This later work occurs in |resolveDurationFormatInternals|; steps not noted
 * here occur there.
 */
function InitializeDurationFormat(durationFormat, locales, options) {
  assert(
    IsObject(durationFormat),
    "InitializeDurationFormat called with non-object"
  );
  assert(
    intl_GuardToDurationFormat(durationFormat) !== null,
    "InitializeDurationFormat called with non-DurationFormat"
  );

  // Lazy DurationFormat data has the following structure:
  //
  //   {
  //     requestedLocales: List of locales,
  //     style: "long" / "short" / "narrow" / "digital",
  //
  //     yearsStyle: "long" / "short" / "narrow",
  //     yearsDisplay: "auto" / "always",
  //
  //     monthsStyle: "long" / "short" / "narrow",
  //     monthsDisplay: "auto" / "always",
  //
  //     weeksStyle: "long" / "short" / "narrow",
  //     weeksDisplay: "auto" / "always",
  //
  //     daysStyle: "long" / "short" / "narrow",
  //     daysDisplay: "auto" / "always",
  //
  //     hoursStyle: "long" / "short" / "narrow" / "numeric" / "2-digit",
  //     hoursDisplay: "auto" / "always",
  //
  //     minutesStyle: "long" / "short" / "narrow" / "numeric" / "2-digit",
  //     minutesDisplay: "auto" / "always",
  //
  //     secondsStyle: "long" / "short" / "narrow" / "numeric" / "2-digit",
  //     secondsDisplay: "auto" / "always",
  //
  //     millisecondsStyle: "long" / "short" / "narrow" / "numeric",
  //     millisecondsDisplay: "auto" / "always",
  //
  //     microsecondsStyle: "long" / "short" / "narrow" / "numeric",
  //     microsecondsDisplay: "auto" / "always",
  //
  //     nanosecondsStyle: "long" / "short" / "narrow" / "numeric",
  //     nanosecondsDisplay: "auto" / "always",
  //
  //     fractionalDigits: integer ∈ [0, 9] / undefined,
  //
  //     opt: // opt object computed in InitializeDurationFormat
  //       {
  //         localeMatcher: "lookup" / "best fit",
  //
  //         nu: string matching a Unicode extension type, // optional
  //       }
  //   }
  //
  // Note that lazy data is only installed as a final step of initialization,
  // so every DurationFormat lazy data object has *all* these properties,
  // never a subset of them.
  var lazyDurationFormatData = std_Object_create(null);

  // Step 3.
  var requestedLocales = CanonicalizeLocaleList(locales);
  lazyDurationFormatData.requestedLocales = requestedLocales;

  // Step 4.
  if (options === undefined) {
    options = std_Object_create(null);
  } else if (!IsObject(options)) {
    ThrowTypeError(
      JSMSG_OBJECT_REQUIRED,
      options === null ? "null" : typeof options
    );
  }

  // Step 5.
  var matcher = GetOption(
    options,
    "localeMatcher",
    "string",
    ["lookup", "best fit"],
    "best fit"
  );

  // Step 6.
  var numberingSystem = GetOption(
    options,
    "numberingSystem",
    "string",
    undefined,
    undefined
  );

  // Step 7.
  if (numberingSystem !== undefined) {
    numberingSystem = intl_ValidateAndCanonicalizeUnicodeExtensionType(
      numberingSystem,
      "numberingSystem",
      "nu"
    );
  }

  // Step 8.
  var opt = new_Record();
  opt.localeMatcher = matcher;
  opt.nu = numberingSystem;

  lazyDurationFormatData.opt = opt;

  // Compute formatting options.

  // Steps 23-24.
  var style = GetOption(
    options,
    "style",
    "string",
    ["long", "short", "narrow", "digital"],
    "short"
  );
  lazyDurationFormatData.style = style;

  // Step 25. (Not applicable in our implementation)

  // Step 26, unit = "years".
  var yearsOptions = GetDurationUnitOptions(
    "years",
    options,
    style,
    ["long", "short", "narrow"],
    "short",
    /* prevStyle= */ ""
  );
  lazyDurationFormatData.yearsStyle = yearsOptions.style;
  lazyDurationFormatData.yearsDisplay = yearsOptions.display;

  // Step 26, unit = "months".
  var monthsOptions = GetDurationUnitOptions(
    "months",
    options,
    style,
    ["long", "short", "narrow"],
    "short",
    /* prevStyle= */ ""
  );
  lazyDurationFormatData.monthsStyle = monthsOptions.style;
  lazyDurationFormatData.monthsDisplay = monthsOptions.display;

  // Step 26, unit = "weeks".
  var weeksOptions = GetDurationUnitOptions(
    "weeks",
    options,
    style,
    ["long", "short", "narrow"],
    "short",
    /* prevStyle= */ ""
  );
  lazyDurationFormatData.weeksStyle = weeksOptions.style;
  lazyDurationFormatData.weeksDisplay = weeksOptions.display;

  // Step 26, unit = "days".
  var daysOptions = GetDurationUnitOptions(
    "days",
    options,
    style,
    ["long", "short", "narrow"],
    "short",
    /* prevStyle= */ ""
  );
  lazyDurationFormatData.daysStyle = daysOptions.style;
  lazyDurationFormatData.daysDisplay = daysOptions.display;

  // Step 26, unit = "hours".
  var hoursOptions = GetDurationUnitOptions(
    "hours",
    options,
    style,
    ["long", "short", "narrow", "numeric", "2-digit"],
    "numeric",
    /* prevStyle= */ ""
  );
  lazyDurationFormatData.hoursStyle = hoursOptions.style;
  lazyDurationFormatData.hoursDisplay = hoursOptions.display;

  // Step 26, unit = "minutes".
  var minutesOptions = GetDurationUnitOptions(
    "minutes",
    options,
    style,
    ["long", "short", "narrow", "numeric", "2-digit"],
    "numeric",
    hoursOptions.style
  );
  lazyDurationFormatData.minutesStyle = minutesOptions.style;
  lazyDurationFormatData.minutesDisplay = minutesOptions.display;

  // Step 26, unit = "seconds".
  var secondsOptions = GetDurationUnitOptions(
    "seconds",
    options,
    style,
    ["long", "short", "narrow", "numeric", "2-digit"],
    "numeric",
    minutesOptions.style
  );
  lazyDurationFormatData.secondsStyle = secondsOptions.style;
  lazyDurationFormatData.secondsDisplay = secondsOptions.display;

  // Step 26, unit = "milliseconds".
  var millisecondsOptions = GetDurationUnitOptions(
    "milliseconds",
    options,
    style,
    ["long", "short", "narrow", "numeric"],
    "numeric",
    secondsOptions.style
  );
  lazyDurationFormatData.millisecondsStyle = millisecondsOptions.style;
  lazyDurationFormatData.millisecondsDisplay = millisecondsOptions.display;

  // Step 26, unit = "microseconds".
  var microsecondsOptions = GetDurationUnitOptions(
    "microseconds",
    options,
    style,
    ["long", "short", "narrow", "numeric"],
    "numeric",
    millisecondsOptions.style
  );
  lazyDurationFormatData.microsecondsStyle = microsecondsOptions.style;
  lazyDurationFormatData.microsecondsDisplay = microsecondsOptions.display;

  // Step 26, unit = "milliseconds".
  var nanosecondsOptions = GetDurationUnitOptions(
    "nanoseconds",
    options,
    style,
    ["long", "short", "narrow", "numeric"],
    "numeric",
    microsecondsOptions.style
  );
  lazyDurationFormatData.nanosecondsStyle = nanosecondsOptions.style;
  lazyDurationFormatData.nanosecondsDisplay = nanosecondsOptions.display;

  // Step 27.
  lazyDurationFormatData.fractionalDigits = GetNumberOption(
    options,
    "fractionalDigits",
    0,
    9,
    undefined
  );

  // We've done everything that must be done now: mark the lazy data as fully
  // computed and install it.
  initializeIntlObject(
    durationFormat,
    "DurationFormat",
    lazyDurationFormatData
  );
}

/**
 * GetDurationUnitOptions ( unit, options, baseStyle, stylesList, digitalBase, prevStyle, twoDigitHours )
 */
function GetDurationUnitOptions(
  unit,
  options,
  baseStyle,
  stylesList,
  digitalBase,
  prevStyle
) {
  assert(typeof unit === "string", "unit is a string");
  assert(IsObject(options), "options is an object");
  assert(typeof baseStyle === "string", "baseStyle is a string");
  assert(IsArray(stylesList), "stylesList is an array");
  assert(typeof digitalBase === "string", "digitalBase is a string");
  assert(typeof prevStyle === "string", "prevStyle is a string");

  // Step 1.
  var styleOption = GetOption(options, unit, "string", stylesList, undefined);

  var style = styleOption;

  // Step 2.
  var displayDefault = "always";

  // Step 3.
  if (style === undefined) {
    // Steps 3.a-b.
    if (baseStyle === "digital") {
      // Step 3.a.i.
      if (unit !== "hours" && unit !== "minutes" && unit !== "seconds") {
        displayDefault = "auto";
      }

      // Step 3.a.ii.
      style = digitalBase;
    } else {
      // Steps 3.b.i-ii. ("fractional" handled implicitly)
      if (prevStyle === "numeric" || prevStyle === "2-digit") {
        // Step 3.b.i.1.
        if (unit !== "minutes" && unit !== "seconds") {
          // Step 3.b.i.1.a.
          displayDefault = "auto";
        }

        // Step 3.b.i.2.
        style = "numeric";
      } else {
        // Step 3.b.ii.1.
        displayDefault = "auto";

        // Step 3.b.ii.2.
        style = baseStyle;
      }
    }
  }

  // Step 4.
  var isFractional =
    style === "numeric" &&
    (unit === "milliseconds" ||
      unit === "microseconds" ||
      unit === "nanoseconds");
  if (isFractional) {
    // Step 4.a.i. (Not applicable in our implementation)

    // Step 4.a.ii.
    displayDefault = "auto";
  }

  // Step 5.
  var displayField = unit + "Display";

  // Step 6.
  var displayOption = GetOption(
    options,
    displayField,
    "string",
    ["auto", "always"],
    undefined
  );

  var display = displayOption ?? displayDefault;

  // Step 7.
  if (display === "always" && isFractional) {
    assert(
      styleOption !== undefined || displayOption !== undefined,
      "no error is thrown when both 'style' and 'display' are absent"
    );

    ThrowRangeError(
      // eslint-disable-next-line no-nested-ternary
      styleOption !== undefined && displayOption !== undefined
        ? JSMSG_INTL_DURATION_INVALID_DISPLAY_OPTION
        : displayOption !== undefined
        ? JSMSG_INTL_DURATION_INVALID_DISPLAY_OPTION_DEFAULT_STYLE
        : JSMSG_INTL_DURATION_INVALID_DISPLAY_OPTION_DEFAULT_DISPLAY,
      unit
    );
  }

  // Steps 8-9.
  if (prevStyle === "numeric" || prevStyle === "2-digit") {
    // Step 8.a. and 9.a.
    if (style !== "numeric" && style !== "2-digit") {
      ThrowRangeError(
        JSMSG_INTL_DURATION_INVALID_NON_NUMERIC_OPTION,
        unit,
        `"${style}"`
      );
    }

    // Step 9.b.
    else if (unit === "minutes" || unit === "seconds") {
      style = "2-digit";
    }
  }

  // Step 10. (Our implementation doesn't use |twoDigitHours|.)

  // Step 11.
  return { style, display };
}

/**
 * Returns the subset of the given locale list for which this locale list has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 */
function Intl_DurationFormat_supportedLocalesOf(locales /*, options*/) {
  var options = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  // Step 1.
  var availableLocales = "DurationFormat";

  // Step 2.
  var requestedLocales = CanonicalizeLocaleList(locales);

  // Step 3.
  return SupportedLocales(availableLocales, requestedLocales, options);
}

/**
 * ToIntegerIfIntegral ( argument )
 */
function ToIntegerIfIntegral(argument) {
  // Step 1.
  var number = ToNumber(argument);

  // Step 2.
  if (!Number_isInteger(number)) {
    ThrowRangeError(JSMSG_INTL_DURATION_NOT_INTEGER, number);
  }

  // Step 3.
  //
  // Add +0 to normalize -0 to +0.
  return number + 0;
}

/**
 * ToDurationRecord ( input )
 */
function ToDurationRecord(input) {
  // Step 1.
  if (!IsObject(input)) {
    // Step 1.a.
    if (typeof input === "string") {
      ThrowRangeError(JSMSG_INTL_DURATION_UNEXPECTED_STRING);
    }

    // Step 1.b.
    ThrowTypeError(
      JSMSG_OBJECT_REQUIRED,
      input === null ? "null" : typeof input
    );
  }

  // Step 2.
  var result = {
    years: 0,
    months: 0,
    weeks: 0,
    days: 0,
    hours: 0,
    minutes: 0,
    seconds: 0,
    milliseconds: 0,
    microseconds: 0,
    nanoseconds: 0,
  };

  // Step 3.
  var days = input.days;

  // Step 4.
  if (days !== undefined) {
    result.days = ToIntegerIfIntegral(days);
  }

  // Step 5.
  var hours = input.hours;

  // Step 6.
  if (hours !== undefined) {
    result.hours = ToIntegerIfIntegral(hours);
  }

  // Step 7.
  var microseconds = input.microseconds;

  // Step 8.
  if (microseconds !== undefined) {
    result.microseconds = ToIntegerIfIntegral(microseconds);
  }

  // Step 9.
  var milliseconds = input.milliseconds;

  // Step 10.
  if (milliseconds !== undefined) {
    result.milliseconds = ToIntegerIfIntegral(milliseconds);
  }

  // Step 11.
  var minutes = input.minutes;

  // Step 12.
  if (minutes !== undefined) {
    result.minutes = ToIntegerIfIntegral(minutes);
  }

  // Step 13.
  var months = input.months;

  // Step 14.
  if (months !== undefined) {
    result.months = ToIntegerIfIntegral(months);
  }

  // Step 15.
  var nanoseconds = input.nanoseconds;

  // Step 16.
  if (nanoseconds !== undefined) {
    result.nanoseconds = ToIntegerIfIntegral(nanoseconds);
  }

  // Step 17.
  var seconds = input.seconds;

  // Step 18.
  if (seconds !== undefined) {
    result.seconds = ToIntegerIfIntegral(seconds);
  }

  // Step 19.
  var weeks = input.weeks;

  // Step 20.
  if (weeks !== undefined) {
    result.weeks = ToIntegerIfIntegral(weeks);
  }

  // Step 21.
  var years = input.years;

  // Step 22.
  if (years !== undefined) {
    result.years = ToIntegerIfIntegral(years);
  }

  // Step 23.
  if (
    years === undefined &&
    months === undefined &&
    weeks === undefined &&
    days === undefined &&
    hours === undefined &&
    minutes === undefined &&
    seconds === undefined &&
    milliseconds === undefined &&
    microseconds === undefined &&
    nanoseconds === undefined
  ) {
    ThrowTypeError(JSMSG_INTL_DURATION_MISSING_UNIT);
  }

  // Step 24.
  if (!IsValidDurationSign(result)) {
    ThrowRangeError(JSMSG_INTL_DURATION_INVALID_SIGN);
  }
  if (!IsValidDurationLimit(result)) {
    ThrowRangeError(JSMSG_INTL_DURATION_INVALID_LIMIT);
  }

  // Step 25.
  return result;
}

/**
 * DurationSign ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds )
 */
function DurationSign(record) {
  assert(IsObject(record), "record is an object");
  assert(Number_isInteger(record.years), "record.years is an integer");
  assert(Number_isInteger(record.months), "record.months is an integer");
  assert(Number_isInteger(record.weeks), "record.weeks is an integer");
  assert(Number_isInteger(record.days), "record.days is an integer");
  assert(Number_isInteger(record.hours), "record.hours is an integer");
  assert(Number_isInteger(record.minutes), "record.minutes is an integer");
  assert(Number_isInteger(record.seconds), "record.seconds is an integer");
  assert(
    Number_isInteger(record.milliseconds),
    "record.milliseconds is an integer"
  );
  assert(
    Number_isInteger(record.microseconds),
    "record.microseconds is an integer"
  );
  assert(
    Number_isInteger(record.nanoseconds),
    "record.nanoseconds is an integer"
  );

  // Steps 1-2. (Loop unrolled)
  return (
    std_Math_sign(record.years) ||
    std_Math_sign(record.months) ||
    std_Math_sign(record.weeks) ||
    std_Math_sign(record.days) ||
    std_Math_sign(record.hours) ||
    std_Math_sign(record.minutes) ||
    std_Math_sign(record.seconds) ||
    std_Math_sign(record.milliseconds) ||
    std_Math_sign(record.microseconds) ||
    std_Math_sign(record.nanoseconds)
  );
}

/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds )
 */
function IsValidDurationSign(record) {
  // Step 1.
  var sign = DurationSign(record);

  // Fast-path for an all-zero duration.
  if (sign === 0) {
    return true;
  }

  // Step 2. (Loop unrolled)
  if (
    std_Math_sign(record.years) === -sign ||
    std_Math_sign(record.months) === -sign ||
    std_Math_sign(record.weeks) === -sign ||
    std_Math_sign(record.days) === -sign ||
    std_Math_sign(record.hours) === -sign ||
    std_Math_sign(record.minutes) === -sign ||
    std_Math_sign(record.seconds) === -sign ||
    std_Math_sign(record.milliseconds) === -sign ||
    std_Math_sign(record.microseconds) === -sign ||
    std_Math_sign(record.nanoseconds) === -sign
  ) {
    return false;
  }

  // Steps 3-8. (Performed in caller)

  // Step 9.
  return true;
}

/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds )
 */
function IsValidDurationLimit(record) {
  // Steps 1-2. (Performed in caller)
  assert(IsValidDurationSign(record), "duration has the correct sign");

  // Steps 3-5.
  if (
    std_Math_abs(record.years) >= 2**32 ||
    std_Math_abs(record.months) >= 2**32 ||
    std_Math_abs(record.weeks) >= 2**32
  ) {
    return false;
  }

  // Steps 6-7.
  var normalizedSeconds = (
    record.days * 86_400 +
    record.hours * 3600 +
    record.minutes * 60 +
    record.seconds
  );

  var milliseconds = record.milliseconds;
  var microseconds = record.microseconds;
  var nanoseconds = record.nanoseconds;
  if (
    std_Math_abs(milliseconds) < 2 ** 53 &&
    std_Math_abs(microseconds) < 2 ** 53 &&
    std_Math_abs(nanoseconds) < 2 ** 53
  ) {
    // Fast case for safe integers.
    normalizedSeconds += (
      std_Math_trunc(milliseconds / 1e3) +
      std_Math_trunc(microseconds / 1e6) +
      std_Math_trunc(nanoseconds / 1e9)
    );
  } else {
    // Slow case for too large numbers.
    normalizedSeconds += BigIntToNumber(
      NumberToBigInt(milliseconds) / 1_000n +
      NumberToBigInt(microseconds) / 1_000_000n +
      NumberToBigInt(nanoseconds) / 1_000_000_000n
    );
  }
  normalizedSeconds += std_Math_trunc((
    (milliseconds % 1e3) * 1e6 +
    (microseconds % 1e6) * 1e3 +
    (nanoseconds % 1e9)
  ) / 1e9);

  // Step 8.
  if (std_Math_abs(normalizedSeconds) >= 2**53) {
    return false;
  }

  // Step 9.
  return true;
}

#ifdef DEBUG
/**
 * IsValidDuration ( years, months, weeks, days, hours, minutes, seconds, milliseconds, microseconds, nanoseconds )
 */
function IsValidDuration(record) {
  return IsValidDurationSign(record) && IsValidDurationLimit(record);
}
#endif

/**
 * ComputeFractionalDigits ( durationFormat, duration )
 *
 * Return the fractional seconds from |duration| as an exact value. This is
 * either an integer Number value when the fractional part is zero, or a
 * decimal string when the fractional part is non-zero.
 */
function ComputeFractionalDigits(duration, unit) {
  assert(
    IsValidDuration(duration),
    "DurationToFractional called with non-valid duration"
  );

  var exponent;
  if (unit === "seconds") {
    exponent = 9;
  } else if (unit === "milliseconds") {
    exponent = 6;
  } else {
    assert(unit === "microseconds", "unexpected unit");
    exponent = 3;
  }

  // Directly return the duration amount when no sub-seconds are present.
  switch (exponent) {
    case 9: {
      if (
        duration.milliseconds === 0 &&
        duration.microseconds === 0 &&
        duration.nanoseconds === 0
      ) {
        return duration.seconds;
      }
      break;
    }

    case 6: {
      if (duration.microseconds === 0 && duration.nanoseconds === 0) {
        return duration.milliseconds;
      }
      break;
    }

    case 3: {
      if (duration.nanoseconds === 0) {
        return duration.microseconds;
      }
      break;
    }
  }

  // Otherwise compute the overall amount of nanoseconds using BigInt to avoid
  // loss of precision.
  var ns = NumberToBigInt(duration.nanoseconds);
  switch (exponent) {
    case 9:
      ns += NumberToBigInt(duration.seconds) * 1_000_000_000n;
      // fallthrough
    case 6:
      ns += NumberToBigInt(duration.milliseconds) * 1_000_000n;
      // fallthrough
    case 3:
      ns += NumberToBigInt(duration.microseconds) * 1_000n;
  }

  var e = NumberToBigInt(10 ** exponent);

  // Split the nanoseconds amount into an integer and its fractional part.
  var q = ns / e;
  var r = ns % e;

  // Pad fractional part, without any leading negative sign, to |exponent| digits.
  if (r < 0) {
    r = -r;
  }
  r = callFunction(String_pad_start, ToString(r), exponent, "0");

  // Return the result as a decimal string.
  return `${q}.${r}`;
}

/**
 * FormatNumericHours ( durationFormat, hoursValue, signDisplayed )
 * FormatNumericMinutes ( durationFormat, minutesValue, hoursDisplayed, signDisplayed )
 * FormatNumericSeconds ( durationFormat, secondsValue, minutesDisplayed, signDisplayed )
 */
function FormatNumericHoursOrMinutesOrSeconds(
  internals,
  value,
  style,
  unit,
  signDisplayed,
  formatToParts
) {
  assert(
    unit === "hour" || unit === "minute" || unit === "second",
    "unexpected unit: " + unit
  );

  // FormatNumericHours, step 1. (Not applicable in our implementation.)
  // FormatNumericMinutes, steps 1-3. (Not applicable in our implementation.)
  // FormatNumericSeconds, step 1. (Not applicable in our implementation.)

  // FormatNumericHours, step 2.
  // FormatNumericMinutes, step 4.
  // FormatNumericSeconds, step 2.
  assert(style === "numeric" || style === "2-digit", "invalid style: " + style);

  // FormatNumericHours, step 3. (Not applicable in our implementation.)
  // FormatNumericSeconds, steps 3-4. (Not applicable in our implementation.)

  // FormatNumericHours, step 4.
  // FormatNumericMinutes, step 5.
  // FormatNumericSeconds, step 5.
  var nfOpts = std_Object_create(null);

  // FormatNumericHours, step 5-6.
  // FormatNumericMinutes, steps 6-7.
  // FormatNumericSeconds, steps 6-7.
  nfOpts.numberingSystem = internals.numberingSystem;

  // FormatNumericHours, step 7.
  // FormatNumericMinutes, step 8.
  // FormatNumericSeconds, step 8.
  if (style === "2-digit") {
    nfOpts.minimumIntegerDigits = 2;
  }

  // FormatNumericHours, step 8.
  // FormatNumericMinutes, step 9.
  // FormatNumericSeconds, step 9.
  if (!signDisplayed) {
    nfOpts.signDisplay = "never";
  }

  // FormatNumericHours, step 9.
  // FormatNumericMinutes, step 10.
  // FormatNumericSeconds, step 10.
  nfOpts.useGrouping = false;

  if (unit === "second") {
    // FormatNumericSeconds, steps 11-14.
    var fractionalDigits = internals.fractionalDigits;
    nfOpts.maximumFractionDigits = fractionalDigits ?? 9;
    nfOpts.minimumFractionDigits = fractionalDigits ?? 0;

    // FormatNumericSeconds, step 15.
    nfOpts.roundingMode = "trunc";
  }

  // FormatNumericHours, step 10.
  // FormatNumericMinutes, step 11.
  // FormatNumericSeconds, step 16.
  var nf = intl_NumberFormat(internals.locale, nfOpts);

  // FormatNumericHours, step 11.
  // FormatNumericMinutes, step 12.
  // FormatNumericSeconds, step 17.
  var parts = intl_FormatNumber(nf, value, formatToParts);

  // FormatNumericHours, step 12.
  // FormatNumericMinutes, step 13.
  // FormatNumericSeconds, step 18.
  if (formatToParts) {
    assert(IsArray(parts), "parts is an array");

    // Modify the parts in-place instead of creating new objects.
    for (var i = 0; i < parts.length; i++) {
      DefineDataProperty(parts[i], "unit", unit);
    }
  }

  // FormatNumericHours, step 13.
  // FormatNumericMinutes, step 14.
  // FormatNumericSeconds, step 19.
  return parts;
}

/* eslint-disable complexity */
/**
 * FormatNumericUnits ( durationFormat, duration, firstNumericUnit, signDisplayed )
 */
function FormatNumericUnits(
  internals,
  duration,
  firstNumericUnit,
  signDisplayed,
  formatToParts
) {
  // Step 1.
  assert(
    firstNumericUnit === "hours" ||
      firstNumericUnit === "minutes" ||
      firstNumericUnit === "seconds",
    "invalid numeric unit: " + firstNumericUnit
  );

  // Step 2.
  var numericPartsList;
  if (formatToParts) {
    numericPartsList = [];
  } else {
    numericPartsList = "";
  }

  // Step 3.
  var hoursValue = duration.hours;

  // Step 4.
  var hoursDisplay = internals.hoursDisplay;

  // Step 5.
  var minutesValue = duration.minutes;

  // Step 6.
  var minutesDisplay = internals.minutesDisplay;

  // Steps 7-8.
  var secondsValue = ComputeFractionalDigits(duration, "seconds");

  // Step 9.
  var secondsDisplay = internals.secondsDisplay;

  // Step 10.
  var hoursFormatted = false;

  // Step 11.
  if (firstNumericUnit === "hours") {
    // Step 11.a.
    hoursFormatted = hoursValue !== 0 || hoursDisplay === "always";
  }

  // Steps 12-13.
  var secondsFormatted = secondsValue !== 0 || secondsDisplay === "always";

  // Step 14.
  var minutesFormatted = false;

  // Step 15.
  if (firstNumericUnit === "hours" || firstNumericUnit === "minutes") {
    // Steps 15.a-b.
    minutesFormatted = (
      (hoursFormatted && secondsFormatted) ||
      minutesValue !== 0 ||
      minutesDisplay === "always"
    );
  }

  // Return early when no units are displayed.
  if (!hoursFormatted && !minutesFormatted && !secondsFormatted) {
    return undefined;
  }

  var timeSeparator;
  if (
    (minutesFormatted && hoursFormatted) ||
    (secondsFormatted && minutesFormatted)
  ) {
    timeSeparator = intl_GetTimeSeparator(
      internals.locale,
      internals.numberingSystem
    );
  }

  // Step 16.
  if (hoursFormatted) {
    // Step 16.a.
    if (signDisplayed && hoursValue === 0 && DurationSign(duration) < 0) {
      hoursValue = -0;
    }

    // Step 16.b.
    var hoursParts = FormatNumericHoursOrMinutesOrSeconds(
      internals,
      hoursValue,
      internals.hoursStyle,
      "hour",
      signDisplayed,
      formatToParts
    );
    if (formatToParts) {
      for (var i = 0; i < hoursParts.length; i++) {
        DefineDataProperty(numericPartsList, numericPartsList.length, hoursParts[i]);
      }
    } else {
      numericPartsList += hoursParts;
    }

    // Step 16.c.
    signDisplayed = false;
  }

  // Step 17.
  if (minutesFormatted) {
    // Step 17.a.
    if (signDisplayed && minutesValue === 0 && DurationSign(duration) < 0) {
      minutesValue = -0;
    }

    // Step 17.b.
    if (hoursFormatted) {
      if (formatToParts) {
        DefineDataProperty(numericPartsList, numericPartsList.length, {
          type: "literal",
          value: timeSeparator,
        });
      } else {
        numericPartsList += timeSeparator;
      }
    }

    var minutesParts = FormatNumericHoursOrMinutesOrSeconds(
      internals,
      minutesValue,
      internals.minutesStyle,
      "minute",
      signDisplayed,
      formatToParts
    );
    if (formatToParts) {
      for (var i = 0; i < minutesParts.length; i++) {
        DefineDataProperty(numericPartsList, numericPartsList.length, minutesParts[i]);
      }
    } else {
      numericPartsList += minutesParts;
    }

    // Step 17.c.
    signDisplayed = false;
  }

  // Step 18.
  if (secondsFormatted) {
    // Step 18.a.
    if (minutesFormatted) {
      if (formatToParts) {
        DefineDataProperty(numericPartsList, numericPartsList.length, {
          type: "literal",
          value: timeSeparator,
        });
      } else {
        numericPartsList += timeSeparator;
      }
    }

    var secondsParts = FormatNumericHoursOrMinutesOrSeconds(
      internals,
      secondsValue,
      internals.secondsStyle,
      "second",
      signDisplayed,
      formatToParts
    );
    if (formatToParts) {
      for (var i = 0; i < secondsParts.length; i++) {
        DefineDataProperty(numericPartsList, numericPartsList.length, secondsParts[i]);
      }
    } else {
      numericPartsList += secondsParts;
    }
  }

  // Step 19.
  return numericPartsList;
}
/* eslint-enable complexity */

/**
 * ListFormatParts ( durationFormat, partitionedPartsList )
 */
function ListFormatParts(internals, partitionedPartsList, formatToParts) {
  assert(IsArray(partitionedPartsList), "partitionedPartsList is an array");

  // Step 1.
  var lfOpts = std_Object_create(null);

  // Step 2.
  lfOpts.type = "unit";

  // Step 3.
  var listStyle = internals.style;

  // Step 4.
  if (listStyle === "digital") {
    listStyle = "short";
  }

  // Step 5.
  lfOpts.style = listStyle;

  // Step 6.
  var ListFormat = GetBuiltinConstructor("ListFormat");
  var lf = new ListFormat(internals.locale, lfOpts);

  // Steps 7-14.
  if (!formatToParts) {
    return intl_FormatList(lf, partitionedPartsList, /* formatToParts = */ false);
  }

  // <https://unicode.org/reports/tr35/tr35-general.html#ListPatterns> requires
  // that the list patterns are sorted, for example "{1} and {0}" isn't a valid
  // pattern, because "{1}" appears before "{0}". This requirement also means
  // all entries appear in order in the formatted result.

  // Step 7.
  var strings = [];

  // Step 8.
  for (var i = 0; i < partitionedPartsList.length; i++) {
    var parts = partitionedPartsList[i];
    assert(IsArray(parts), "parts is an array");

    // Step 8.a.
    var string = "";

    // Step 8.b.
    //
    // Combine the individual number-formatted parts into a single string.
    for (var j = 0; j < parts.length; j++) {
      var part = parts[j];
      assert(
        hasOwn("type", part) &&
          hasOwn("value", part) &&
          typeof part.value === "string",
        "part is a number-formatted element"
      );

      string += part.value;
    }

    // Step 8.c.
    DefineDataProperty(strings, strings.length, string);
  }

  // Step 9.
  var formattedPartsList = intl_FormatList(lf, strings, /* formatToParts = */ true);

  // Step 10.
  var partitionedPartsIndex = 0;

  // Step 11. (Not applicable in our implementation.)

  // Step 12.
  var flattenedPartsList = [];

  // Step 13.
  for (var i = 0; i < formattedPartsList.length; i++) {
    var listPart = formattedPartsList[i];
    assert(
      hasOwn("type", listPart) &&
        hasOwn("value", listPart) &&
        typeof listPart.type === "string",
      "part is a list-formatted element"
    );

    // Steps 13.a-b.
    if (listPart.type === "element") {
      // Step 13.a.i.
      assert(
        partitionedPartsIndex < partitionedPartsList.length,
        "resultIndex is an index into result"
      );

      // Step 13.a.ii.
      var parts = partitionedPartsList[partitionedPartsIndex];
      assert(IsArray(parts), "parts is an array");

      // Step 13.a.iii.
      //
      // Replace "element" parts with the number-formatted result.
      for (var j = 0; j < parts.length; j++) {
        DefineDataProperty(flattenedPartsList, flattenedPartsList.length, parts[j]);
      }

      // Step 13.a.iv.
      partitionedPartsIndex += 1;
    } else {
      // Step 13.b.i.
      assert(listPart.type === "literal", "literal part");

      // Step 13.b.ii.
      //
      // Append "literal" parts as-is.
      DefineDataProperty(flattenedPartsList, flattenedPartsList.length, listPart);
    }
  }
  assert(
    partitionedPartsIndex === partitionedPartsList.length,
    "all number-formatted parts handled"
  );

  // Step 14.
  return flattenedPartsList;
}

/**
 * PartitionDurationFormatPattern ( durationFormat, duration )
 */
function PartitionDurationFormatPattern(
  durationFormat,
  duration,
  formatToParts
) {
  assert(
    IsObject(durationFormat),
    "PartitionDurationFormatPattern called with non-object"
  );
  assert(
    intl_GuardToDurationFormat(durationFormat) !== null,
    "PartitionDurationFormatPattern called with non-DurationFormat"
  );
  assert(
    IsValidDuration(duration),
    "PartitionDurationFormatPattern called with non-valid duration"
  );
  assert(
    typeof formatToParts === "boolean",
    "PartitionDurationFormatPattern called with non-boolean formatToParts"
  );

  var units = [
    "years",
    "months",
    "weeks",
    "days",
    "hours",
    "minutes",
    "seconds",
    "milliseconds",
    "microseconds",
    "nanoseconds",
  ];

  var internals = getDurationFormatInternals(durationFormat);

  // Step 1.
  var result = [];

  // Step 2.
  var signDisplayed = true;

  // Step 3.
  var numericUnitFound = false;

  // Step 4.
  for (var i = 0; !numericUnitFound && i < units.length; i++) {
    // Step 4.d. (Reordered)
    var unit = units[i];

    // Step 4.a.
    var value = duration[unit];

    // Step 4.b.
    var style = internals[unit + "Style"];

    // Step 4.c.
    var display = internals[unit + "Display"];

    // Step 4.d. (Moved above)

    // Steps 4.e-f.
    if (style === "numeric" || style === "2-digit") {
      // Step 4.e.1.
      var numericPartsList = FormatNumericUnits(
        internals,
        duration,
        unit,
        signDisplayed,
        formatToParts
      );
      if (numericPartsList !== undefined) {
        DefineDataProperty(result, result.length, numericPartsList);
      }

      // Step 4.e.2.
      numericUnitFound = true;
    } else {
      // Step 4.f.i.
      var nfOpts = std_Object_create(null);

      // Step 4.f.ii.
      if (
        unit === "seconds" ||
        unit === "milliseconds" ||
        unit === "microseconds"
      ) {
        // Step 4.f.ii.1.
        if (internals[units[i + 1] + "Style"] === "numeric") {
          // Step 4.f.ii.1.a.
          value = ComputeFractionalDigits(duration, unit);

          // Steps 4.f.ii.1.b-e.
          var fractionalDigits = internals.fractionalDigits;
          nfOpts.maximumFractionDigits = fractionalDigits ?? 9;
          nfOpts.minimumFractionDigits = fractionalDigits ?? 0;

          // Step 4.f.ii.1.f.
          nfOpts.roundingMode = "trunc";

          // Step 4.f.ii.1.g.
          numericUnitFound = true;
        }
      }

      // Step 4.f.iii. (Condition inverted to reduce indentation.)
      if (value === 0 && display === "auto") {
        continue;
      }

      // Steps 4.f.iii.1-2.
      nfOpts.numberingSystem = internals.numberingSystem;

      // Steps 4.f.iii.3-4.
      if (signDisplayed) {
        // Step 4.f.iii.3.a.
        signDisplayed = false;

        // Step 4.f.iii.3.b.
        if (value === 0 && DurationSign(duration) < 0) {
          value = -0;
        }
      } else {
        // Step 4.f.iii.4.b.
        nfOpts.signDisplay = "never";
      }

      // Step 4.f.iii.5.
      //
      // Remove the trailing 's' from the unit name.
      var numberFormatUnit = Substring(unit, 0, unit.length - 1);

      // Step 4.f.iii.6.
      nfOpts.style = "unit";

      // Step 4.f.iii.7.
      nfOpts.unit = numberFormatUnit;

      // Step 4.f.iii.8.
      nfOpts.unitDisplay = style;

      // Step 4.f.iii.9.
      var nf = intl_NumberFormat(internals.locale, nfOpts);

      // Step 4.f.iii.10.
      var parts = intl_FormatNumber(nf, value, formatToParts);

      // Steps 4.f.iii.11-12.
      if (formatToParts) {
        assert(IsArray(parts), "parts is an array");

        // Modify the parts in-place instead of creating new objects.
        for (var j = 0; j < parts.length; j++) {
          DefineDataProperty(parts[j], "unit", numberFormatUnit);
        }
      }

      // Step 4.f.iii.13.
      DefineDataProperty(result, result.length, parts);
    }
  }

  // Step 5.
  return ListFormatParts(internals, result, formatToParts);
}

/**
 * Intl.DurationFormat.prototype.format ( durationLike )
 */
function Intl_DurationFormat_format(durationLike) {
  // Step 1.
  var df = this;

  // Step 2.
  if (!IsObject(df) || (df = intl_GuardToDurationFormat(df)) === null) {
    return callFunction(
      intl_CallDurationFormatMethodIfWrapped,
      this,
      durationLike,
      "Intl_DurationFormat_format"
    );
  }

  // Step 3.
  var duration = ToTemporalDuration(durationLike);

  // Fallback if Temporal is disabled.
  if (duration === null) {
    duration = ToDurationRecord(durationLike);
  }

  // Steps 4-7.
  return PartitionDurationFormatPattern(
    df,
    duration,
    /* formatToParts = */ false
  );
}

/**
 * Intl.DurationFormat.prototype.formatToParts ( durationLike )
 */
function Intl_DurationFormat_formatToParts(durationLike) {
  // Step 1.
  var df = this;

  // Step 2.
  if (!IsObject(df) || (df = intl_GuardToDurationFormat(df)) === null) {
    return callFunction(
      intl_CallDurationFormatMethodIfWrapped,
      this,
      durationLike,
      "Intl_DurationFormat_formatToParts"
    );
  }

  // Step 3.
  var duration = ToTemporalDuration(durationLike);

  // Fallback if Temporal is disabled.
  if (duration === null) {
    duration = ToDurationRecord(durationLike);
  }

  // Steps 4-8.
  return PartitionDurationFormatPattern(df, duration, /* formatToParts = */ true);
}

/**
 * Returns the resolved options for a DurationFormat object.
 */
function Intl_DurationFormat_resolvedOptions() {
  // Step 1.
  var durationFormat = this;

  // Step 2.
  if (
    !IsObject(durationFormat) ||
    (durationFormat = intl_GuardToDurationFormat(durationFormat)) === null
  ) {
    return callFunction(
      intl_CallDurationFormatMethodIfWrapped,
      this,
      "Intl_DurationFormat_resolvedOptions"
    );
  }

  var internals = getDurationFormatInternals(durationFormat);

  // Steps 3-4.
  var result = {
    locale: internals.locale,
    numberingSystem: internals.numberingSystem,
    style: internals.style,
    years: internals.yearsStyle,
    yearsDisplay: internals.yearsDisplay,
    months: internals.monthsStyle,
    monthsDisplay: internals.monthsDisplay,
    weeks: internals.weeksStyle,
    weeksDisplay: internals.weeksDisplay,
    days: internals.daysStyle,
    daysDisplay: internals.daysDisplay,
    hours: internals.hoursStyle,
    hoursDisplay: internals.hoursDisplay,
    minutes: internals.minutesStyle,
    minutesDisplay: internals.minutesDisplay,
    seconds: internals.secondsStyle,
    secondsDisplay: internals.secondsDisplay,
    milliseconds: internals.millisecondsStyle,
    millisecondsDisplay: internals.millisecondsDisplay,
    microseconds: internals.microsecondsStyle,
    microsecondsDisplay: internals.microsecondsDisplay,
    nanoseconds: internals.nanosecondsStyle,
    nanosecondsDisplay: internals.nanosecondsDisplay,
  };

  if (internals.fractionalDigits !== undefined) {
    DefineDataProperty(result, "fractionalDigits", internals.fractionalDigits);
  }

  // Step 5.
  return result;
}
