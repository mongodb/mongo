/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Portions Copyright Norbert Lindenberg 2011-2012. */

#include "NumberingSystemsGenerated.h"

/**
 * NumberFormat internal properties.
 *
 * 9.1 Internal slots of Service Constructors
 * 15.2.3 Properties of the Intl.NumberFormat Constructor, Internal slots
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
var numberFormatInternalProperties = {
  localeData: numberFormatLocaleData,
  relevantExtensionKeys: ["nu"],
};

/**
 * 15.1.1 Intl.NumberFormat ( [ locales [ , options ] ] )
 *
 * Compute an internal properties object from |lazyNumberFormatData|.
 *
 * ES2025 Intl draft rev 5ea95f8a98d660e94c177d6f5e88c6d2962123b1
 */
function resolveNumberFormatInternals(lazyNumberFormatData) {
  assert(IsObject(lazyNumberFormatData), "lazy data not an object?");

  var internalProps = std_Object_create(null);

  var NumberFormat = numberFormatInternalProperties;

  // Compute effective locale.

  // Step 11.
  var r = ResolveLocale(
    "NumberFormat",
    lazyNumberFormatData.requestedLocales,
    lazyNumberFormatData.opt,
    NumberFormat.relevantExtensionKeys,
    NumberFormat.localeData
  );

  // Steps 12-14. (Step 13 is not relevant to our implementation.)
  internalProps.locale = r.locale;
  internalProps.numberingSystem = r.nu;

  // Compute formatting options.

  // Step 15. SetNumberFormatUnitOptions, step 2.
  var style = lazyNumberFormatData.style;
  internalProps.style = style;

  // Step 15. SetNumberFormatUnitOptions, step 12.
  if (style === "currency") {
    internalProps.currency = lazyNumberFormatData.currency;
    internalProps.currencyDisplay = lazyNumberFormatData.currencyDisplay;
    internalProps.currencySign = lazyNumberFormatData.currencySign;
  }

  // Step 15. SetNumberFormatUnitOptions, step 13.
  if (style === "unit") {
    internalProps.unit = lazyNumberFormatData.unit;
    internalProps.unitDisplay = lazyNumberFormatData.unitDisplay;
  }

  // Step 18.
  var notation = lazyNumberFormatData.notation;
  internalProps.notation = notation;

  // Step 21. SetNumberFormatDigitOptions, step 6.
  internalProps.minimumIntegerDigits =
    lazyNumberFormatData.minimumIntegerDigits;

  // Step 21. SetNumberFormatDigitOptions, step 14.
  internalProps.roundingIncrement = lazyNumberFormatData.roundingIncrement;

  // Step 21. SetNumberFormatDigitOptions, step 15.
  internalProps.roundingMode = lazyNumberFormatData.roundingMode;

  // Step 21. SetNumberFormatDigitOptions, step 16.
  internalProps.trailingZeroDisplay = lazyNumberFormatData.trailingZeroDisplay;

  // Step 21. SetNumberFormatDigitOptions, steps 25-26.
  if ("minimumFractionDigits" in lazyNumberFormatData) {
    // Note: Intl.NumberFormat.prototype.resolvedOptions() exposes the
    // actual presence (versus undefined-ness) of these properties.
    assert(
      "maximumFractionDigits" in lazyNumberFormatData,
      "min/max frac digits mismatch"
    );
    internalProps.minimumFractionDigits =
      lazyNumberFormatData.minimumFractionDigits;
    internalProps.maximumFractionDigits =
      lazyNumberFormatData.maximumFractionDigits;
  }

  // Step 21. SetNumberFormatDigitOptions, steps 24 and 26.
  if ("minimumSignificantDigits" in lazyNumberFormatData) {
    // Note: Intl.NumberFormat.prototype.resolvedOptions() exposes the
    // actual presence (versus undefined-ness) of these properties.
    assert(
      "maximumSignificantDigits" in lazyNumberFormatData,
      "min/max sig digits mismatch"
    );
    internalProps.minimumSignificantDigits =
      lazyNumberFormatData.minimumSignificantDigits;
    internalProps.maximumSignificantDigits =
      lazyNumberFormatData.maximumSignificantDigits;
  }

  // Step 21. SetNumberFormatDigitOptions, steps 26-30.
  internalProps.roundingPriority = lazyNumberFormatData.roundingPriority;

  // Step 24.
  if (notation === "compact") {
    internalProps.compactDisplay = lazyNumberFormatData.compactDisplay;
  }

  // Step 29.
  internalProps.useGrouping = lazyNumberFormatData.useGrouping;

  // Step 31.
  internalProps.signDisplay = lazyNumberFormatData.signDisplay;

  // The caller is responsible for associating |internalProps| with the right
  // object using |setInternalProperties|.
  return internalProps;
}

/**
 * Returns an object containing the NumberFormat internal properties of |obj|.
 */
function getNumberFormatInternals(obj) {
  assert(IsObject(obj), "getNumberFormatInternals called with non-object");
  assert(
    intl_GuardToNumberFormat(obj) !== null,
    "getNumberFormatInternals called with non-NumberFormat"
  );

  var internals = getIntlObjectInternals(obj);
  assert(
    internals.type === "NumberFormat",
    "bad type escaped getIntlObjectInternals"
  );

  // If internal properties have already been computed, use them.
  var internalProps = maybeInternalProperties(internals);
  if (internalProps) {
    return internalProps;
  }

  // Otherwise it's time to fully create them.
  internalProps = resolveNumberFormatInternals(internals.lazyData);
  setInternalProperties(internals, internalProps);
  return internalProps;
}

/**
 * 15.5.10 UnwrapNumberFormat ( nf )
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function UnwrapNumberFormat(nf) {
  // Steps 1-3 (error handling moved to caller).
  if (
    IsObject(nf) &&
    intl_GuardToNumberFormat(nf) === null &&
    !intl_IsWrappedNumberFormat(nf) &&
    callFunction(
      std_Object_isPrototypeOf,
      GetBuiltinPrototype("NumberFormat"),
      nf
    )
  ) {
    return nf[intlFallbackSymbol()];
  }
  return nf;
}

/* eslint-disable complexity */
/**
 * 15.1.3 SetNumberFormatDigitOptions ( intlObj, options, mnfdDefault, mxfdDefault, notation )
 *
 * Applies digit options used for number formatting onto the intl object.
 *
 * ES2024 Intl draft rev a1db4567870dbe505121a4255f1210338757190a
 */
function SetNumberFormatDigitOptions(
  lazyData,
  options,
  mnfdDefault,
  mxfdDefault,
  notation
) {
  assert(IsObject(options), "SetNumberFormatDigitOptions");
  assert(typeof mnfdDefault === "number", "SetNumberFormatDigitOptions");
  assert(typeof mxfdDefault === "number", "SetNumberFormatDigitOptions");
  assert(mnfdDefault <= mxfdDefault, "SetNumberFormatDigitOptions");
  assert(typeof notation === "string", "SetNumberFormatDigitOptions");

  // Steps 1-5.
  var mnid = GetNumberOption(options, "minimumIntegerDigits", 1, 21, 1);
  var mnfd = options.minimumFractionDigits;
  var mxfd = options.maximumFractionDigits;
  var mnsd = options.minimumSignificantDigits;
  var mxsd = options.maximumSignificantDigits;

  // Step 6.
  lazyData.minimumIntegerDigits = mnid;

  // Step 7.
  var roundingIncrement = GetNumberOption(
    options,
    "roundingIncrement",
    1,
    5000,
    1
  );

  // Step 8.
  switch (roundingIncrement) {
    case 1:
    case 2:
    case 5:
    case 10:
    case 20:
    case 25:
    case 50:
    case 100:
    case 200:
    case 250:
    case 500:
    case 1000:
    case 2000:
    case 2500:
    case 5000:
      break;
    default:
      ThrowRangeError(
        JSMSG_INVALID_OPTION_VALUE,
        "roundingIncrement",
        roundingIncrement
      );
  }

  // Step 9.
  var roundingMode = GetOption(
    options,
    "roundingMode",
    "string",
    [
      "ceil",
      "floor",
      "expand",
      "trunc",
      "halfCeil",
      "halfFloor",
      "halfExpand",
      "halfTrunc",
      "halfEven",
    ],
    "halfExpand"
  );

  // Step 10.
  var roundingPriority = GetOption(
    options,
    "roundingPriority",
    "string",
    ["auto", "morePrecision", "lessPrecision"],
    "auto"
  );

  // Step 11.
  var trailingZeroDisplay = GetOption(
    options,
    "trailingZeroDisplay",
    "string",
    ["auto", "stripIfInteger"],
    "auto"
  );

  // Step 12. (This step is a note.)

  // Step 13.
  if (roundingIncrement !== 1) {
    mxfdDefault = mnfdDefault;
  }

  // Step 14.
  lazyData.roundingIncrement = roundingIncrement;

  // Step 15.
  lazyData.roundingMode = roundingMode;

  // Step 16.
  lazyData.trailingZeroDisplay = trailingZeroDisplay;

  // Step 17.
  var hasSignificantDigits = mnsd !== undefined || mxsd !== undefined;

  // Step 28.
  var hasFractionDigits = mnfd !== undefined || mxfd !== undefined;

  // Steps 19 and 21.a.
  var needSignificantDigits =
    roundingPriority !== "auto" || hasSignificantDigits;

  // Steps 20 and 21.b.i.
  var needFractionalDigits =
    roundingPriority !== "auto" ||
    !(hasSignificantDigits || (!hasFractionDigits && notation === "compact"));

  // Step 22.
  if (needSignificantDigits) {
    // Step 22.a.
    if (hasSignificantDigits) {
      // Step 22.a.i.
      mnsd = DefaultNumberOption(mnsd, 1, 21, 1);
      lazyData.minimumSignificantDigits = mnsd;

      // Step 22.a.ii.
      mxsd = DefaultNumberOption(mxsd, mnsd, 21, 21);
      lazyData.maximumSignificantDigits = mxsd;
    } else {
      // Step 22.b.i.
      lazyData.minimumSignificantDigits = 1;

      // Step 22.b.ii.
      lazyData.maximumSignificantDigits = 21;
    }
  }

  // Step 23.
  if (needFractionalDigits) {
    // Step 23.a.
    if (hasFractionDigits) {
      // Step 23.a.i.
      mnfd = DefaultNumberOption(mnfd, 0, 100, undefined);

      // Step 23.a.ii.
      mxfd = DefaultNumberOption(mxfd, 0, 100, undefined);

      // Step 23.a.iii.
      if (mnfd === undefined) {
        assert(
          mxfd !== undefined,
          "mxfd isn't undefined when mnfd is undefined"
        );
        mnfd = std_Math_min(mnfdDefault, mxfd);
      }

      // Step 23.a.iv.
      else if (mxfd === undefined) {
        mxfd = std_Math_max(mxfdDefault, mnfd);
      }

      // Step 23.a.v.
      else if (mnfd > mxfd) {
        ThrowRangeError(JSMSG_INVALID_DIGITS_VALUE, mxfd);
      }

      // Step 23.a.vi.
      lazyData.minimumFractionDigits = mnfd;

      // Step 23.a.vii.
      lazyData.maximumFractionDigits = mxfd;
    } else {
      // Step 23.b.i.
      lazyData.minimumFractionDigits = mnfdDefault;

      // Step 23.b.ii.
      lazyData.maximumFractionDigits = mxfdDefault;
    }
  }

  // Steps 24-28.
  if (!needSignificantDigits && !needFractionalDigits) {
    assert(!hasSignificantDigits, "bad significant digits in fallback case");
    assert(
      roundingPriority === "auto",
      `bad rounding in fallback case: ${roundingPriority}`
    );
    assert(
      notation === "compact",
      `bad notation in fallback case: ${notation}`
    );

    // Steps 24.a-f.
    lazyData.minimumFractionDigits = 0;
    lazyData.maximumFractionDigits = 0;
    lazyData.minimumSignificantDigits = 1;
    lazyData.maximumSignificantDigits = 2;
    lazyData.roundingPriority = "morePrecision";
  } else {
    // Steps 25-28.
    //
    // Our implementation stores |roundingPriority| instead of using
    // [[RoundingType]].
    lazyData.roundingPriority = roundingPriority;
  }

  // Step 29.
  if (roundingIncrement !== 1) {
    // Step 29.a.
    //
    // [[RoundingType]] is `fractionDigits` if |roundingPriority| is equal to
    // "auto" and |hasSignificantDigits| is false.
    if (roundingPriority !== "auto") {
      ThrowTypeError(
        JSMSG_INVALID_NUMBER_OPTION,
        "roundingIncrement",
        "roundingPriority"
      );
    }
    if (hasSignificantDigits) {
      ThrowTypeError(
        JSMSG_INVALID_NUMBER_OPTION,
        "roundingIncrement",
        "minimumSignificantDigits"
      );
    }

    // Step 29.b.
    //
    // Minimum and maximum fraction digits must be equal.
    if (
      lazyData.minimumFractionDigits !==
      lazyData.maximumFractionDigits
    ) {
      ThrowRangeError(JSMSG_UNEQUAL_FRACTION_DIGITS);
    }
  }
}
/* eslint-enable complexity */

/**
 * Convert s to upper case, but limited to characters a-z.
 *
 * Spec: ECMAScript Internationalization API Specification, 6.1.
 */
function toASCIIUpperCase(s) {
  assert(typeof s === "string", "toASCIIUpperCase");

  // String.prototype.toUpperCase may map non-ASCII characters into ASCII,
  // so go character by character (actually code unit by code unit, but
  // since we only care about ASCII characters here, that's OK).
  var result = "";
  for (var i = 0; i < s.length; i++) {
    var c = callFunction(std_String_charCodeAt, s, i);
    result +=
      0x61 <= c && c <= 0x7a
        ? callFunction(std_String_fromCharCode, null, c & ~0x20)
        : s[i];
  }
  return result;
}

/**
 * 6.3.1 IsWellFormedCurrencyCode ( currency )
 *
 * Verifies that the given string is a well-formed ISO 4217 currency code.
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function IsWellFormedCurrencyCode(currency) {
  assert(typeof currency === "string", "currency is a string value");

  return currency.length === 3 && IsASCIIAlphaString(currency);
}

/**
 * 6.6.1 IsWellFormedUnitIdentifier ( unitIdentifier )
 *
 * Verifies that the given string is a well-formed core unit identifier as
 * defined in UTS #35, Part 2, Section 6. In addition to obeying the UTS #35
 * core unit identifier syntax, |unitIdentifier| must be one of the identifiers
 * sanctioned by UTS #35 or be a compound unit composed of two sanctioned simple
 * units.
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function IsWellFormedUnitIdentifier(unitIdentifier) {
  assert(
    typeof unitIdentifier === "string",
    "unitIdentifier is a string value"
  );

  // Step 1.
  if (IsSanctionedSimpleUnitIdentifier(unitIdentifier)) {
    return true;
  }

  // Steps 2-3.
  var pos = callFunction(std_String_indexOf, unitIdentifier, "-per-");
  if (pos < 0) {
    return false;
  }

  // Step 4.
  //
  // Sanctioned single unit identifiers don't include the substring "-per-",
  // so we can skip searching for the second "-per-" substring.

  var next = pos + "-per-".length;

  // Steps 5-6.
  var numerator = Substring(unitIdentifier, 0, pos);
  var denominator = Substring(
    unitIdentifier,
    next,
    unitIdentifier.length - next
  );

  // Steps 7-8.
  return (
    IsSanctionedSimpleUnitIdentifier(numerator) &&
    IsSanctionedSimpleUnitIdentifier(denominator)
  );
}

#if DEBUG || MOZ_SYSTEM_ICU
var availableMeasurementUnits = {
  value: null,
};
#endif

/**
 * 6.6.2 IsSanctionedSingleUnitIdentifier ( unitIdentifier )
 *
 * Verifies that the given string is a sanctioned simple core unit identifier.
 *
 * Also see: https://unicode.org/reports/tr35/tr35-general.html#Unit_Elements
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function IsSanctionedSimpleUnitIdentifier(unitIdentifier) {
  assert(
    typeof unitIdentifier === "string",
    "unitIdentifier is a string value"
  );

  var isSanctioned = hasOwn(unitIdentifier, sanctionedSimpleUnitIdentifiers);

#if DEBUG || MOZ_SYSTEM_ICU
  if (isSanctioned) {
    if (availableMeasurementUnits.value === null) {
      availableMeasurementUnits.value = intl_availableMeasurementUnits();
    }

    var isSupported = hasOwn(unitIdentifier, availableMeasurementUnits.value);

#if MOZ_SYSTEM_ICU
    // A system ICU may support fewer measurement units, so we need to make
    // sure the unit is actually supported.
    isSanctioned = isSupported;
#else
    // Otherwise just assert that the sanctioned unit is also supported.
    assert(
      isSupported,
      `"${unitIdentifier}" is sanctioned but not supported. Did you forget to update
      intl/icu/data_filter.json to include the unit (and any implicit compound units)?
      For example "speed/kilometer-per-hour" is implied by "length/kilometer" and
      "duration/hour" and must therefore also be present.`
    );
#endif
  }
#endif

  return isSanctioned;
}

/* eslint-disable complexity */
/**
 * 15.1.1 Intl.NumberFormat ( [ locales [ , options ] ] )
 *
 * Initializes an object as a NumberFormat.
 *
 * This method is complicated a moderate bit by its implementing initialization
 * as a *lazy* concept.  Everything that must happen now, does -- but we defer
 * all the work we can until the object is actually used as a NumberFormat.
 * This later work occurs in |resolveNumberFormatInternals|; steps not noted
 * here occur there.
 *
 * ES2025 Intl draft rev 5ea95f8a98d660e94c177d6f5e88c6d2962123b1
 */
function InitializeNumberFormat(numberFormat, thisValue, locales, options) {
  assert(
    IsObject(numberFormat),
    "InitializeNumberFormat called with non-object"
  );
  assert(
    intl_GuardToNumberFormat(numberFormat) !== null,
    "InitializeNumberFormat called with non-NumberFormat"
  );

  // Lazy NumberFormat data has the following structure:
  //
  //   {
  //     requestedLocales: List of locales,
  //     style: "decimal" / "percent" / "currency" / "unit",
  //
  //     // fields present only if style === "currency":
  //     currency: a well-formed currency code (IsWellFormedCurrencyCode),
  //     currencyDisplay: "code" / "symbol" / "narrowSymbol" / "name",
  //     currencySign: "standard" / "accounting",
  //
  //     // fields present only if style === "unit":
  //     unit: a well-formed unit identifier (IsWellFormedUnitIdentifier),
  //     unitDisplay: "short" / "narrow" / "long",
  //
  //     opt: // opt object computed in InitializeNumberFormat
  //       {
  //         localeMatcher: "lookup" / "best fit",
  //
  //         nu: string matching a Unicode extension type, // optional
  //       }
  //
  //     minimumIntegerDigits: integer ∈ [1, 21],
  //
  //     // optional, mutually exclusive with the significant-digits option
  //     minimumFractionDigits: integer ∈ [0, 100],
  //     maximumFractionDigits: integer ∈ [0, 100],
  //
  //     // optional, mutually exclusive with the fraction-digits option
  //     minimumSignificantDigits: integer ∈ [1, 21],
  //     maximumSignificantDigits: integer ∈ [1, 21],
  //
  //     roundingPriority: "auto" / "lessPrecision" / "morePrecision",
  //
  //     useGrouping: "auto" / "always" / "min2" / false,
  //
  //     notation: "standard" / "scientific" / "engineering" / "compact",
  //
  //     // optional, if notation is "compact"
  //     compactDisplay: "short" / "long",
  //
  //     signDisplay: "auto" / "never" / "always" / "exceptZero" / "negative",
  //
  //     trailingZeroDisplay: "auto" / "stripIfInteger",
  //
  //     roundingIncrement: integer ∈ (1, 2, 5,
  //                                   10, 20, 25, 50,
  //                                   100, 200, 250, 500,
  //                                   1000, 2000, 2500, 5000),
  //
  //     roundingMode: "ceil" / "floor" / "expand" / "trunc" /
  //                   "halfCeil" / "halfFloor" / "halfExpand" / "halfTrunc" / "halfEven",
  //   }
  //
  // Note that lazy data is only installed as a final step of initialization,
  // so every NumberFormat lazy data object has *all* these properties, never a
  // subset of them.
  var lazyNumberFormatData = std_Object_create(null);

  // Step 3.
  var requestedLocales = CanonicalizeLocaleList(locales);
  lazyNumberFormatData.requestedLocales = requestedLocales;

  // Step 4. (Inlined call to CoerceOptionsToObject.)
  //
  // If we ever need more speed here at startup, we should try to detect the
  // case where |options === undefined| and then directly use the default
  // value for each option.  For now, just keep it simple.
  if (options === undefined) {
    options = std_Object_create(null);
  } else {
    options = ToObject(options);
  }

  // Compute options that impact interpretation of locale.

  // Step 5.
  var opt = new_Record();
  lazyNumberFormatData.opt = opt;

  // Steps 6-7.
  var matcher = GetOption(
    options,
    "localeMatcher",
    "string",
    ["lookup", "best fit"],
    "best fit"
  );
  opt.localeMatcher = matcher;

  // Step 8.
  var numberingSystem = GetOption(
    options,
    "numberingSystem",
    "string",
    undefined,
    undefined
  );

  // Step 9.
  if (numberingSystem !== undefined) {
    numberingSystem = intl_ValidateAndCanonicalizeUnicodeExtensionType(
      numberingSystem,
      "numberingSystem",
      "nu"
    );
  }

  // Step 10.
  opt.nu = numberingSystem;

  // Compute formatting options.

  // Step 15. SetNumberFormatUnitOptions, steps 1-2.
  var style = GetOption(
    options,
    "style",
    "string",
    ["decimal", "percent", "currency", "unit"],
    "decimal"
  );
  lazyNumberFormatData.style = style;

  // Step 15. SetNumberFormatUnitOptions, step 3.
  var currency = GetOption(options, "currency", "string", undefined, undefined);

  // Step 15. SetNumberFormatUnitOptions, steps 4-5.
  if (currency === undefined) {
    if (style === "currency") {
      ThrowTypeError(JSMSG_UNDEFINED_CURRENCY);
    }
  } else {
    if (!IsWellFormedCurrencyCode(currency)) {
      ThrowRangeError(JSMSG_INVALID_CURRENCY_CODE, currency);
    }
  }

  // Step 15. SetNumberFormatUnitOptions, step 6.
  var currencyDisplay = GetOption(
    options,
    "currencyDisplay",
    "string",
    ["code", "symbol", "narrowSymbol", "name"],
    "symbol"
  );

  // Step 15. SetNumberFormatUnitOptions, step 7.
  var currencySign = GetOption(
    options,
    "currencySign",
    "string",
    ["standard", "accounting"],
    "standard"
  );

  // Step 15. SetNumberFormatUnitOptions, step 12. (Reordered)
  if (style === "currency") {
    // Step 15. SetNumberFormatUnitOptions, step 12.a.
    currency = toASCIIUpperCase(currency);
    lazyNumberFormatData.currency = currency;

    // Step 15. SetNumberFormatUnitOptions, step 12.b.
    lazyNumberFormatData.currencyDisplay = currencyDisplay;

    // Step 15. SetNumberFormatUnitOptions, step 12.c.
    lazyNumberFormatData.currencySign = currencySign;
  }

  // Step 15. SetNumberFormatUnitOptions, step 8.
  var unit = GetOption(options, "unit", "string", undefined, undefined);

  // Step 15. SetNumberFormatUnitOptions, steps 9-10.
  if (unit === undefined) {
    if (style === "unit") {
      ThrowTypeError(JSMSG_UNDEFINED_UNIT);
    }
  } else {
    if (!IsWellFormedUnitIdentifier(unit)) {
      ThrowRangeError(JSMSG_INVALID_UNIT_IDENTIFIER, unit);
    }
  }

  // Step 15. SetNumberFormatUnitOptions, step 11.
  var unitDisplay = GetOption(
    options,
    "unitDisplay",
    "string",
    ["short", "narrow", "long"],
    "short"
  );

  // Step 15. SetNumberFormatUnitOptions, step 13.
  if (style === "unit") {
    lazyNumberFormatData.unit = unit;
    lazyNumberFormatData.unitDisplay = unitDisplay;
  }

  // Step 16. (Not applicable in our implementation.)

  // Steps 17-18.
  var notation = GetOption(
    options,
    "notation",
    "string",
    ["standard", "scientific", "engineering", "compact"],
    "standard"
  );
  lazyNumberFormatData.notation = notation;

  // Steps 19-20.
  var mnfdDefault, mxfdDefault;
  if (style === "currency" && notation === "standard") {
    var cDigits = CurrencyDigits(currency);
    mnfdDefault = cDigits;
    mxfdDefault = cDigits;
  } else {
    mnfdDefault = 0;
    mxfdDefault = style === "percent" ? 0 : 3;
  }

  // Step 21.
  SetNumberFormatDigitOptions(
    lazyNumberFormatData,
    options,
    mnfdDefault,
    mxfdDefault,
    notation
  );

  // Steps 22 and 24.a.
  var compactDisplay = GetOption(
    options,
    "compactDisplay",
    "string",
    ["short", "long"],
    "short"
  );
  if (notation === "compact") {
    lazyNumberFormatData.compactDisplay = compactDisplay;
  }

  // Steps 23 and 24.b.
  var defaultUseGrouping = notation !== "compact" ? "auto" : "min2";

  // Steps 25-26.
  var useGrouping = GetStringOrBooleanOption(
    options,
    "useGrouping",
    ["min2", "auto", "always", "true", "false"],
    defaultUseGrouping
  );

  // Steps 27-28.
  if (useGrouping === "true" || useGrouping === "false") {
    useGrouping = defaultUseGrouping;
  } else if (useGrouping === true) {
    useGrouping = "always";
  }

  // Step 29.
  assert(
    useGrouping === "min2" ||
    useGrouping === "auto" ||
    useGrouping === "always" ||
    useGrouping === false,
    `invalid 'useGrouping' value: ${useGrouping}`
  );
  lazyNumberFormatData.useGrouping = useGrouping;

  // Steps 30-31.
  var signDisplay = GetOption(
    options,
    "signDisplay",
    "string",
    ["auto", "never", "always", "exceptZero", "negative"],
    "auto"
  );
  lazyNumberFormatData.signDisplay = signDisplay;

  // We've done everything that must be done now: mark the lazy data as fully
  // computed and install it.
  initializeIntlObject(numberFormat, "NumberFormat", lazyNumberFormatData);

  // Step 32. (Inlined call to ChainNumberFormat.)
  if (
    numberFormat !== thisValue &&
    callFunction(
      std_Object_isPrototypeOf,
      GetBuiltinPrototype("NumberFormat"),
      thisValue
    )
  ) {
    DefineDataProperty(
      thisValue,
      intlFallbackSymbol(),
      numberFormat,
      ATTR_NONENUMERABLE | ATTR_NONCONFIGURABLE | ATTR_NONWRITABLE
    );

    return thisValue;
  }

  // Step 33.
  return numberFormat;
}
/* eslint-enable complexity */

/**
 * 15.5.1 CurrencyDigits ( currency )
 *
 * Returns the number of decimal digits to be used for the given currency.
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function CurrencyDigits(currency) {
  assert(typeof currency === "string", "currency is a string value");
  assert(IsWellFormedCurrencyCode(currency), "currency is well-formed");
  assert(currency === toASCIIUpperCase(currency), "currency is all upper-case");

  // Step 1.
  if (hasOwn(currency, currencyDigits)) {
    return currencyDigits[currency];
  }
  return 2;
}

/**
 * 15.2.2 Intl.NumberFormat.supportedLocalesOf ( locales [ , options ] )
 *
 * Returns the subset of the given locale list for which this locale list has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function Intl_NumberFormat_supportedLocalesOf(locales /*, options*/) {
  var options = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  // Step 1.
  var availableLocales = "NumberFormat";

  // Step 2.
  var requestedLocales = CanonicalizeLocaleList(locales);

  // Step 3.
  return SupportedLocales(availableLocales, requestedLocales, options);
}

function getNumberingSystems(locale) {
  // ICU doesn't have an API to determine the set of numbering systems
  // supported for a locale; it generally pretends that any numbering system
  // can be used with any locale. Supporting a decimal numbering system
  // (where only the digits are replaced) is easy, so we offer them all here.
  // Algorithmic numbering systems are typically tied to one locale, so for
  // lack of information we don't offer them.
  // The one thing we can find out from ICU is the default numbering system
  // for a locale.
  var defaultNumberingSystem = intl_numberingSystem(locale);
  return [defaultNumberingSystem, NUMBERING_SYSTEMS_WITH_SIMPLE_DIGIT_MAPPINGS];
}

function numberFormatLocaleData() {
  return {
    nu: getNumberingSystems,
    default: {
      nu: intl_numberingSystem,
    },
  };
}

/**
 * 15.5.2 Number Format Functions
 *
 * Create function to be cached and returned by Intl.NumberFormat.prototype.format.
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function createNumberFormatFormat(nf) {
  // This function is not inlined in $Intl_NumberFormat_format_get to avoid
  // creating a call-object on each call to $Intl_NumberFormat_format_get.
  return function(value) {
    // Step 1 (implicit).

    // Step 2.
    assert(IsObject(nf), "InitializeNumberFormat called with non-object");
    assert(
      intl_GuardToNumberFormat(nf) !== null,
      "InitializeNumberFormat called with non-NumberFormat"
    );

    // Steps 3-5.
    return intl_FormatNumber(nf, value, /* formatToParts = */ false);
  };
}

/**
 * 15.3.3 get Intl.NumberFormat.prototype.format
 *
 * Returns a function bound to this NumberFormat that returns a String value
 * representing the result of calling ToNumber(value) according to the
 * effective locale and the formatting options of this NumberFormat.
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
// Uncloned functions with `$` prefix are allocated as extended function
// to store the original name in `SetCanonicalName`.
function $Intl_NumberFormat_format_get() {
  // Steps 1-3.
  var thisArg = UnwrapNumberFormat(this);
  var nf = thisArg;
  if (!IsObject(nf) || (nf = intl_GuardToNumberFormat(nf)) === null) {
    return callFunction(
      intl_CallNumberFormatMethodIfWrapped,
      thisArg,
      "$Intl_NumberFormat_format_get"
    );
  }

  var internals = getNumberFormatInternals(nf);

  // Step 4.
  if (internals.boundFormat === undefined) {
    // Steps 4.a-c.
    internals.boundFormat = createNumberFormatFormat(nf);
  }

  // Step 5.
  return internals.boundFormat;
}
SetCanonicalName($Intl_NumberFormat_format_get, "get format");

/**
 * 15.3.4 Intl.NumberFormat.prototype.formatToParts ( value )
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function Intl_NumberFormat_formatToParts(value) {
  // Step 1.
  var nf = this;

  // Step 2.
  if (!IsObject(nf) || (nf = intl_GuardToNumberFormat(nf)) === null) {
    return callFunction(
      intl_CallNumberFormatMethodIfWrapped,
      this,
      value,
      "Intl_NumberFormat_formatToParts"
    );
  }

  // Steps 3-4.
  return intl_FormatNumber(nf, value, /* formatToParts = */ true);
}

/**
 * 15.3.5 Intl.NumberFormat.prototype.formatRange ( start, end )
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function Intl_NumberFormat_formatRange(start, end) {
  // Step 1.
  var nf = this;

  // Step 2.
  if (!IsObject(nf) || (nf = intl_GuardToNumberFormat(nf)) === null) {
    return callFunction(
      intl_CallNumberFormatMethodIfWrapped,
      this,
      start,
      end,
      "Intl_NumberFormat_formatRange"
    );
  }

  // Step 3.
  if (start === undefined || end === undefined) {
    ThrowTypeError(
      JSMSG_UNDEFINED_NUMBER,
      start === undefined ? "start" : "end",
      "NumberFormat",
      "formatRange"
    );
  }

  // Steps 4-6.
  return intl_FormatNumberRange(nf, start, end, /* formatToParts = */ false);
}

/**
 * 15.3.6 Intl.NumberFormat.prototype.formatRangeToParts ( start, end )
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function Intl_NumberFormat_formatRangeToParts(start, end) {
  // Step 1.
  var nf = this;

  // Step 2.
  if (!IsObject(nf) || (nf = intl_GuardToNumberFormat(nf)) === null) {
    return callFunction(
      intl_CallNumberFormatMethodIfWrapped,
      this,
      start,
      end,
      "Intl_NumberFormat_formatRangeToParts"
    );
  }

  // Step 3.
  if (start === undefined || end === undefined) {
    ThrowTypeError(
      JSMSG_UNDEFINED_NUMBER,
      start === undefined ? "start" : "end",
      "NumberFormat",
      "formatRangeToParts"
    );
  }

  // Steps 4-6.
  return intl_FormatNumberRange(nf, start, end, /* formatToParts = */ true);
}

/**
 * 15.3.7 Intl.NumberFormat.prototype.resolvedOptions ( )
 *
 * Returns the resolved options for a NumberFormat object.
 *
 * ES2024 Intl draft rev a1db4567870dbe505121a4255f1210338757190a
 */
function Intl_NumberFormat_resolvedOptions() {
  // Steps 1-3.
  var thisArg = UnwrapNumberFormat(this);
  var nf = thisArg;
  if (!IsObject(nf) || (nf = intl_GuardToNumberFormat(nf)) === null) {
    return callFunction(
      intl_CallNumberFormatMethodIfWrapped,
      thisArg,
      "Intl_NumberFormat_resolvedOptions"
    );
  }

  var internals = getNumberFormatInternals(nf);

  // Steps 4-5.
  var result = {
    locale: internals.locale,
    numberingSystem: internals.numberingSystem,
    style: internals.style,
  };

  // currency, currencyDisplay, and currencySign are only present for currency
  // formatters.
  assert(
    hasOwn("currency", internals) === (internals.style === "currency"),
    "currency is present iff style is 'currency'"
  );
  assert(
    hasOwn("currencyDisplay", internals) === (internals.style === "currency"),
    "currencyDisplay is present iff style is 'currency'"
  );
  assert(
    hasOwn("currencySign", internals) === (internals.style === "currency"),
    "currencySign is present iff style is 'currency'"
  );

  if (hasOwn("currency", internals)) {
    DefineDataProperty(result, "currency", internals.currency);
    DefineDataProperty(result, "currencyDisplay", internals.currencyDisplay);
    DefineDataProperty(result, "currencySign", internals.currencySign);
  }

  // unit and unitDisplay are only present for unit formatters.
  assert(
    hasOwn("unit", internals) === (internals.style === "unit"),
    "unit is present iff style is 'unit'"
  );
  assert(
    hasOwn("unitDisplay", internals) === (internals.style === "unit"),
    "unitDisplay is present iff style is 'unit'"
  );

  if (hasOwn("unit", internals)) {
    DefineDataProperty(result, "unit", internals.unit);
    DefineDataProperty(result, "unitDisplay", internals.unitDisplay);
  }

  DefineDataProperty(
    result,
    "minimumIntegerDigits",
    internals.minimumIntegerDigits
  );

  // Min/Max fraction digits are either both present or not present at all.
  assert(
    hasOwn("minimumFractionDigits", internals) ===
      hasOwn("maximumFractionDigits", internals),
    "minimumFractionDigits is present iff maximumFractionDigits is present"
  );

  if (hasOwn("minimumFractionDigits", internals)) {
    DefineDataProperty(
      result,
      "minimumFractionDigits",
      internals.minimumFractionDigits
    );
    DefineDataProperty(
      result,
      "maximumFractionDigits",
      internals.maximumFractionDigits
    );
  }

  // Min/Max significant digits are either both present or not present at all.
  assert(
    hasOwn("minimumSignificantDigits", internals) ===
      hasOwn("maximumSignificantDigits", internals),
    "minimumSignificantDigits is present iff maximumSignificantDigits is present"
  );

  if (hasOwn("minimumSignificantDigits", internals)) {
    DefineDataProperty(
      result,
      "minimumSignificantDigits",
      internals.minimumSignificantDigits
    );
    DefineDataProperty(
      result,
      "maximumSignificantDigits",
      internals.maximumSignificantDigits
    );
  }

  DefineDataProperty(result, "useGrouping", internals.useGrouping);

  var notation = internals.notation;
  DefineDataProperty(result, "notation", notation);

  // compactDisplay is only present when `notation` is "compact".
  if (notation === "compact") {
    DefineDataProperty(result, "compactDisplay", internals.compactDisplay);
  }

  DefineDataProperty(result, "signDisplay", internals.signDisplay);
  DefineDataProperty(result, "roundingIncrement", internals.roundingIncrement);
  DefineDataProperty(result, "roundingMode", internals.roundingMode);
  DefineDataProperty(result, "roundingPriority", internals.roundingPriority);
  DefineDataProperty(
    result,
    "trailingZeroDisplay",
    internals.trailingZeroDisplay
  );

  // Step 6.
  return result;
}
