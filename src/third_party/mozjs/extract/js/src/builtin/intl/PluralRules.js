/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * PluralRules internal properties.
 *
 * 9.1 Internal slots of Service Constructors
 * 16.2.3 Properties of the Intl.PluralRules Constructor, Internal slots
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
var pluralRulesInternalProperties = {
  localeData: pluralRulesLocaleData,
  relevantExtensionKeys: [],
};

function pluralRulesLocaleData() {
  // PluralRules don't support any extension keys.
  return {};
}

/**
 * 16.1.2 InitializePluralRules ( pluralRules, locales, options )
 *
 * Compute an internal properties object from |lazyPluralRulesData|.
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function resolvePluralRulesInternals(lazyPluralRulesData) {
  assert(IsObject(lazyPluralRulesData), "lazy data not an object?");

  var internalProps = std_Object_create(null);

  var PluralRules = pluralRulesInternalProperties;

  // Compute effective locale.

  // Step 9.
  var localeData = PluralRules.localeData;

  // Step 10.
  var r = ResolveLocale(
    "PluralRules",
    lazyPluralRulesData.requestedLocales,
    lazyPluralRulesData.opt,
    PluralRules.relevantExtensionKeys,
    localeData
  );

  // Step 11.
  internalProps.locale = r.locale;

  // Step 7.
  internalProps.type = lazyPluralRulesData.type;

  // Step 8. SetNumberFormatDigitOptions, step 6.
  internalProps.minimumIntegerDigits = lazyPluralRulesData.minimumIntegerDigits;

  // Step 8. SetNumberFormatDigitOptions, step 14.
  internalProps.roundingIncrement = lazyPluralRulesData.roundingIncrement;

  // Step 8. SetNumberFormatDigitOptions, step 15.
  internalProps.roundingMode = lazyPluralRulesData.roundingMode;

  // Step 8. SetNumberFormatDigitOptions, step 16.
  internalProps.trailingZeroDisplay = lazyPluralRulesData.trailingZeroDisplay;

  // Step 8. SetNumberFormatDigitOptions, steps 25-26.
  if ("minimumFractionDigits" in lazyPluralRulesData) {
    assert(
      "maximumFractionDigits" in lazyPluralRulesData,
      "min/max frac digits mismatch"
    );
    internalProps.minimumFractionDigits =
      lazyPluralRulesData.minimumFractionDigits;
    internalProps.maximumFractionDigits =
      lazyPluralRulesData.maximumFractionDigits;
  }

  // Step 8. SetNumberFormatDigitOptions, steps 24 and 26.
  if ("minimumSignificantDigits" in lazyPluralRulesData) {
    assert(
      "maximumSignificantDigits" in lazyPluralRulesData,
      "min/max sig digits mismatch"
    );
    internalProps.minimumSignificantDigits =
      lazyPluralRulesData.minimumSignificantDigits;
    internalProps.maximumSignificantDigits =
      lazyPluralRulesData.maximumSignificantDigits;
  }

  // Step 8. SetNumberFormatDigitOptions, steps 26-30.
  internalProps.roundingPriority = lazyPluralRulesData.roundingPriority;

  // `pluralCategories` is lazily computed on first access.
  internalProps.pluralCategories = null;

  return internalProps;
}

/**
 * Returns an object containing the PluralRules internal properties of |obj|.
 */
function getPluralRulesInternals(obj) {
  assert(IsObject(obj), "getPluralRulesInternals called with non-object");
  assert(
    intl_GuardToPluralRules(obj) !== null,
    "getPluralRulesInternals called with non-PluralRules"
  );

  var internals = getIntlObjectInternals(obj);
  assert(
    internals.type === "PluralRules",
    "bad type escaped getIntlObjectInternals"
  );

  var internalProps = maybeInternalProperties(internals);
  if (internalProps) {
    return internalProps;
  }

  internalProps = resolvePluralRulesInternals(internals.lazyData);
  setInternalProperties(internals, internalProps);
  return internalProps;
}

/**
 * 16.1.2 InitializePluralRules ( pluralRules, locales, options )
 *
 * Initializes an object as a PluralRules.
 *
 * This method is complicated a moderate bit by its implementing initialization
 * as a *lazy* concept.  Everything that must happen now, does -- but we defer
 * all the work we can until the object is actually used as a PluralRules.
 * This later work occurs in |resolvePluralRulesInternals|; steps not noted
 * here occur there.
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function InitializePluralRules(pluralRules, locales, options) {
  assert(IsObject(pluralRules), "InitializePluralRules called with non-object");
  assert(
    intl_GuardToPluralRules(pluralRules) !== null,
    "InitializePluralRules called with non-PluralRules"
  );

  // Lazy PluralRules data has the following structure:
  //
  //   {
  //     requestedLocales: List of locales,
  //     type: "cardinal" / "ordinal",
  //
  //     opt: // opt object computer in InitializePluralRules
  //       {
  //         localeMatcher: "lookup" / "best fit",
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
  // so every PluralRules lazy data object has *all* these properties, never a
  // subset of them.
  var lazyPluralRulesData = std_Object_create(null);

  // Step 1.
  var requestedLocales = CanonicalizeLocaleList(locales);
  lazyPluralRulesData.requestedLocales = requestedLocales;

  // Step 2. (Inlined call to CoerceOptionsToObject.)
  if (options === undefined) {
    options = std_Object_create(null);
  } else {
    options = ToObject(options);
  }

  // Step 3.
  var opt = new_Record();
  lazyPluralRulesData.opt = opt;

  // Steps 4-5.
  var matcher = GetOption(
    options,
    "localeMatcher",
    "string",
    ["lookup", "best fit"],
    "best fit"
  );
  opt.localeMatcher = matcher;

  // Steps 6-7.
  var type = GetOption(
    options,
    "type",
    "string",
    ["cardinal", "ordinal"],
    "cardinal"
  );
  lazyPluralRulesData.type = type;

  // Step 8.
  SetNumberFormatDigitOptions(lazyPluralRulesData, options, 0, 3, "standard");

  // Step 12.
  //
  // We've done everything that must be done now: mark the lazy data as fully
  // computed and install it.
  initializeIntlObject(pluralRules, "PluralRules", lazyPluralRulesData);
}

/**
 * 16.2.2 Intl.PluralRules.supportedLocalesOf ( locales [ , options ] )
 *
 * Returns the subset of the given locale list for which this locale list has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function Intl_PluralRules_supportedLocalesOf(locales /*, options*/) {
  var options = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  // Step 1.
  var availableLocales = "PluralRules";

  // Step 2.
  var requestedLocales = CanonicalizeLocaleList(locales);

  // Step 3.
  return SupportedLocales(availableLocales, requestedLocales, options);
}

/**
 * 16.3.3 Intl.PluralRules.prototype.select ( value )
 *
 * Returns a String value representing the plural category matching
 * the number passed as value according to the
 * effective locale and the formatting options of this PluralRules.
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function Intl_PluralRules_select(value) {
  // Step 1.
  var pluralRules = this;

  // Step 2.
  if (
    !IsObject(pluralRules) ||
    (pluralRules = intl_GuardToPluralRules(pluralRules)) === null
  ) {
    return callFunction(
      intl_CallPluralRulesMethodIfWrapped,
      this,
      value,
      "Intl_PluralRules_select"
    );
  }

  // Step 3.
  var n = ToNumber(value);

  // Ensure the PluralRules internals are resolved.
  getPluralRulesInternals(pluralRules);

  // Step 4.
  return intl_SelectPluralRule(pluralRules, n);
}

/**
 * 16.3.4 Intl.PluralRules.prototype.selectRange ( start, end )
 *
 * Returns a String value representing the plural category matching the input
 * number range according to the effective locale and the formatting options
 * of this PluralRules.
 *
 * ES2024 Intl draft rev 74ca7099f103d143431b2ea422ae640c6f43e3e6
 */
function Intl_PluralRules_selectRange(start, end) {
  // Step 1.
  var pluralRules = this;

  // Step 2.
  if (
    !IsObject(pluralRules) ||
    (pluralRules = intl_GuardToPluralRules(pluralRules)) === null
  ) {
    return callFunction(
      intl_CallPluralRulesMethodIfWrapped,
      this,
      start,
      end,
      "Intl_PluralRules_selectRange"
    );
  }

  // Step 3.
  if (start === undefined || end === undefined) {
    ThrowTypeError(
      JSMSG_UNDEFINED_NUMBER,
      start === undefined ? "start" : "end",
      "PluralRules",
      "selectRange"
    );
  }

  // Step 4.
  var x = ToNumber(start);

  // Step 5.
  var y = ToNumber(end);

  // Step 6.
  return intl_SelectPluralRuleRange(pluralRules, x, y);
}

/**
 * 16.3.5 Intl.PluralRules.prototype.resolvedOptions ( )
 *
 * Returns the resolved options for a PluralRules object.
 *
 * ES2024 Intl draft rev a1db4567870dbe505121a4255f1210338757190a
 */
function Intl_PluralRules_resolvedOptions() {
  // Step 1.
  var pluralRules = this;

  // Step 2.
  if (
    !IsObject(pluralRules) ||
    (pluralRules = intl_GuardToPluralRules(pluralRules)) === null
  ) {
    return callFunction(
      intl_CallPluralRulesMethodIfWrapped,
      this,
      "Intl_PluralRules_resolvedOptions"
    );
  }

  var internals = getPluralRulesInternals(pluralRules);

  // Step 4.
  var internalsPluralCategories = internals.pluralCategories;
  if (internalsPluralCategories === null) {
    internalsPluralCategories = intl_GetPluralCategories(pluralRules);
    internals.pluralCategories = internalsPluralCategories;
  }

  // Step 5.b.
  var pluralCategories = [];
  for (var i = 0; i < internalsPluralCategories.length; i++) {
    DefineDataProperty(pluralCategories, i, internalsPluralCategories[i]);
  }

  // Steps 3 and 5.
  var result = {
    locale: internals.locale,
    type: internals.type,
    minimumIntegerDigits: internals.minimumIntegerDigits,
  };

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

  DefineDataProperty(result, "pluralCategories", pluralCategories);
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
