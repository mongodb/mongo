/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * RelativeTimeFormat internal properties.
 *
 * Spec: ECMAScript 402 API, RelativeTimeFormat, 1.3.3.
 */
var relativeTimeFormatInternalProperties = {
  localeData: relativeTimeFormatLocaleData,
  relevantExtensionKeys: ["nu"],
};

function relativeTimeFormatLocaleData() {
  return {
    nu: getNumberingSystems,
    default: {
      nu: intl_numberingSystem,
    },
  };
}

/**
 * Compute an internal properties object from |lazyRelativeTimeFormatData|.
 */
function resolveRelativeTimeFormatInternals(lazyRelativeTimeFormatData) {
  assert(IsObject(lazyRelativeTimeFormatData), "lazy data not an object?");

  var internalProps = std_Object_create(null);

  var RelativeTimeFormat = relativeTimeFormatInternalProperties;

  // Steps 10-11.
  var r = ResolveLocale(
    "RelativeTimeFormat",
    lazyRelativeTimeFormatData.requestedLocales,
    lazyRelativeTimeFormatData.opt,
    RelativeTimeFormat.relevantExtensionKeys,
    RelativeTimeFormat.localeData
  );

  // Steps 12-13.
  internalProps.locale = r.locale;

  // Step 14.
  internalProps.numberingSystem = r.nu;

  // Step 15 (Not relevant in our implementation).

  // Step 17.
  internalProps.style = lazyRelativeTimeFormatData.style;

  // Step 19.
  internalProps.numeric = lazyRelativeTimeFormatData.numeric;

  // Steps 20-24 (Not relevant in our implementation).

  return internalProps;
}

/**
 * Returns an object containing the RelativeTimeFormat internal properties of |obj|.
 */
function getRelativeTimeFormatInternals(obj) {
  assert(
    IsObject(obj),
    "getRelativeTimeFormatInternals called with non-object"
  );
  assert(
    intl_GuardToRelativeTimeFormat(obj) !== null,
    "getRelativeTimeFormatInternals called with non-RelativeTimeFormat"
  );

  var internals = getIntlObjectInternals(obj);
  assert(
    internals.type === "RelativeTimeFormat",
    "bad type escaped getIntlObjectInternals"
  );

  var internalProps = maybeInternalProperties(internals);
  if (internalProps) {
    return internalProps;
  }

  internalProps = resolveRelativeTimeFormatInternals(internals.lazyData);
  setInternalProperties(internals, internalProps);
  return internalProps;
}

/**
 * Initializes an object as a RelativeTimeFormat.
 *
 * This method is complicated a moderate bit by its implementing initialization
 * as a *lazy* concept.  Everything that must happen now, does -- but we defer
 * all the work we can until the object is actually used as a RelativeTimeFormat.
 * This later work occurs in |resolveRelativeTimeFormatInternals|; steps not noted
 * here occur there.
 *
 * Spec: ECMAScript 402 API, RelativeTimeFormat, 1.1.1.
 */
function InitializeRelativeTimeFormat(relativeTimeFormat, locales, options) {
  assert(
    IsObject(relativeTimeFormat),
    "InitializeRelativeimeFormat called with non-object"
  );
  assert(
    intl_GuardToRelativeTimeFormat(relativeTimeFormat) !== null,
    "InitializeRelativeTimeFormat called with non-RelativeTimeFormat"
  );

  // Lazy RelativeTimeFormat data has the following structure:
  //
  //   {
  //     requestedLocales: List of locales,
  //     style: "long" / "short" / "narrow",
  //     numeric: "always" / "auto",
  //
  //     opt: // opt object computed in InitializeRelativeTimeFormat
  //       {
  //         localeMatcher: "lookup" / "best fit",
  //       }
  //   }
  //
  // Note that lazy data is only installed as a final step of initialization,
  // so every RelativeTimeFormat lazy data object has *all* these properties, never a
  // subset of them.
  var lazyRelativeTimeFormatData = std_Object_create(null);

  // Step 1.
  var requestedLocales = CanonicalizeLocaleList(locales);
  lazyRelativeTimeFormatData.requestedLocales = requestedLocales;

  // Steps 2-3.
  if (options === undefined) {
    options = std_Object_create(null);
  } else {
    options = ToObject(options);
  }

  // Step 4.
  var opt = new_Record();

  // Steps 5-6.
  var matcher = GetOption(
    options,
    "localeMatcher",
    "string",
    ["lookup", "best fit"],
    "best fit"
  );
  opt.localeMatcher = matcher;

  // Steps 7-9.
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
  opt.nu = numberingSystem;

  lazyRelativeTimeFormatData.opt = opt;

  // Steps 16-17.
  var style = GetOption(
    options,
    "style",
    "string",
    ["long", "short", "narrow"],
    "long"
  );
  lazyRelativeTimeFormatData.style = style;

  // Steps 18-19.
  var numeric = GetOption(
    options,
    "numeric",
    "string",
    ["always", "auto"],
    "always"
  );
  lazyRelativeTimeFormatData.numeric = numeric;

  initializeIntlObject(
    relativeTimeFormat,
    "RelativeTimeFormat",
    lazyRelativeTimeFormatData
  );
}

/**
 * Returns the subset of the given locale list for which this locale list has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 *
 * Spec: ECMAScript 402 API, RelativeTimeFormat, 1.3.2.
 */
function Intl_RelativeTimeFormat_supportedLocalesOf(locales /*, options*/) {
  var options = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  // Step 1.
  var availableLocales = "RelativeTimeFormat";

  // Step 2.
  var requestedLocales = CanonicalizeLocaleList(locales);

  // Step 3.
  return SupportedLocales(availableLocales, requestedLocales, options);
}

/**
 * Returns a String value representing the written form of a relative date
 * formatted according to the effective locale and the formatting options
 * of this RelativeTimeFormat object.
 *
 * Spec: ECMAScript 402 API, RelativeTImeFormat, 1.4.3.
 */
function Intl_RelativeTimeFormat_format(value, unit) {
  // Step 1.
  var relativeTimeFormat = this;

  // Step 2.
  if (
    !IsObject(relativeTimeFormat) ||
    (relativeTimeFormat = intl_GuardToRelativeTimeFormat(
      relativeTimeFormat
    )) === null
  ) {
    return callFunction(
      intl_CallRelativeTimeFormatMethodIfWrapped,
      this,
      value,
      unit,
      "Intl_RelativeTimeFormat_format"
    );
  }

  // Step 3.
  var t = ToNumber(value);

  // Step 4.
  var u = ToString(unit);

  // Step 5.
  return intl_FormatRelativeTime(relativeTimeFormat, t, u, false);
}

/**
 * Returns an Array composed of the components of a relative date formatted
 * according to the effective locale and the formatting options of this
 * RelativeTimeFormat object.
 *
 * Spec: ECMAScript 402 API, RelativeTImeFormat, 1.4.4.
 */
function Intl_RelativeTimeFormat_formatToParts(value, unit) {
  // Step 1.
  var relativeTimeFormat = this;

  // Step 2.
  if (
    !IsObject(relativeTimeFormat) ||
    (relativeTimeFormat = intl_GuardToRelativeTimeFormat(
      relativeTimeFormat
    )) === null
  ) {
    return callFunction(
      intl_CallRelativeTimeFormatMethodIfWrapped,
      this,
      value,
      unit,
      "Intl_RelativeTimeFormat_formatToParts"
    );
  }

  // Step 3.
  var t = ToNumber(value);

  // Step 4.
  var u = ToString(unit);

  // Step 5.
  return intl_FormatRelativeTime(relativeTimeFormat, t, u, true);
}

/**
 * Returns the resolved options for a RelativeTimeFormat object.
 *
 * Spec: ECMAScript 402 API, RelativeTimeFormat, 1.4.5.
 */
function Intl_RelativeTimeFormat_resolvedOptions() {
  // Step 1.
  var relativeTimeFormat = this;

  // Steps 2-3.
  if (
    !IsObject(relativeTimeFormat) ||
    (relativeTimeFormat = intl_GuardToRelativeTimeFormat(
      relativeTimeFormat
    )) === null
  ) {
    return callFunction(
      intl_CallRelativeTimeFormatMethodIfWrapped,
      this,
      "Intl_RelativeTimeFormat_resolvedOptions"
    );
  }

  var internals = getRelativeTimeFormatInternals(relativeTimeFormat);

  // Steps 4-5.
  var result = {
    locale: internals.locale,
    style: internals.style,
    numeric: internals.numeric,
    numberingSystem: internals.numberingSystem,
  };

  // Step 6.
  return result;
}
