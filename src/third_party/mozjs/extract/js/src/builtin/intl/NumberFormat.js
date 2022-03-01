/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Portions Copyright Norbert Lindenberg 2011-2012. */

#include "NumberingSystemsGenerated.h"

/**
 * NumberFormat internal properties.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.1 and 11.3.3.
 */
var numberFormatInternalProperties = {
    localeData: numberFormatLocaleData,
    relevantExtensionKeys: ["nu"],
};

/**
 * Compute an internal properties object from |lazyNumberFormatData|.
 */
function resolveNumberFormatInternals(lazyNumberFormatData) {
    assert(IsObject(lazyNumberFormatData), "lazy data not an object?");

    var internalProps = std_Object_create(null);

    var NumberFormat = numberFormatInternalProperties;

    // Compute effective locale.

    // Step 7.
    var localeData = NumberFormat.localeData;

    // Step 8.
    var r = ResolveLocale("NumberFormat",
                          lazyNumberFormatData.requestedLocales,
                          lazyNumberFormatData.opt,
                          NumberFormat.relevantExtensionKeys,
                          localeData);

    // Steps 9-10. (Step 11 is not relevant to our implementation.)
    internalProps.locale = r.locale;
    internalProps.numberingSystem = r.nu;

    // Compute formatting options.
    // Step 13.
    var style = lazyNumberFormatData.style;
    internalProps.style = style;

    // Steps 17, 19.
    if (style === "currency") {
        internalProps.currency = lazyNumberFormatData.currency;
        internalProps.currencyDisplay = lazyNumberFormatData.currencyDisplay;
        internalProps.currencySign = lazyNumberFormatData.currencySign;
    }

    // Intl.NumberFormat Unified API Proposal
    if (style === "unit") {
        internalProps.unit = lazyNumberFormatData.unit;
        internalProps.unitDisplay = lazyNumberFormatData.unitDisplay;
    }

    // Intl.NumberFormat Unified API Proposal
    var notation = lazyNumberFormatData.notation;
    internalProps.notation = notation;

    // Step 22.
    internalProps.minimumIntegerDigits = lazyNumberFormatData.minimumIntegerDigits;

    if ("minimumFractionDigits" in lazyNumberFormatData) {
        // Note: Intl.NumberFormat.prototype.resolvedOptions() exposes the
        // actual presence (versus undefined-ness) of these properties.
        assert("maximumFractionDigits" in lazyNumberFormatData, "min/max frac digits mismatch");
        internalProps.minimumFractionDigits = lazyNumberFormatData.minimumFractionDigits;
        internalProps.maximumFractionDigits = lazyNumberFormatData.maximumFractionDigits;
    }

    if ("minimumSignificantDigits" in lazyNumberFormatData) {
        // Note: Intl.NumberFormat.prototype.resolvedOptions() exposes the
        // actual presence (versus undefined-ness) of these properties.
        assert("maximumSignificantDigits" in lazyNumberFormatData, "min/max sig digits mismatch");
        internalProps.minimumSignificantDigits = lazyNumberFormatData.minimumSignificantDigits;
        internalProps.maximumSignificantDigits = lazyNumberFormatData.maximumSignificantDigits;
    }

    // Intl.NumberFormat Unified API Proposal
    if (notation === "compact")
        internalProps.compactDisplay = lazyNumberFormatData.compactDisplay;

    // Step 24.
    internalProps.useGrouping = lazyNumberFormatData.useGrouping;

    // Intl.NumberFormat Unified API Proposal
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
    assert(intl_GuardToNumberFormat(obj) !== null,
           "getNumberFormatInternals called with non-NumberFormat");

    var internals = getIntlObjectInternals(obj);
    assert(internals.type === "NumberFormat", "bad type escaped getIntlObjectInternals");

    // If internal properties have already been computed, use them.
    var internalProps = maybeInternalProperties(internals);
    if (internalProps)
        return internalProps;

    // Otherwise it's time to fully create them.
    internalProps = resolveNumberFormatInternals(internals.lazyData);
    setInternalProperties(internals, internalProps);
    return internalProps;
}

/**
 * 11.1.11 UnwrapNumberFormat( nf )
 */
function UnwrapNumberFormat(nf) {
    // Steps 2 and 4 (error handling moved to caller).
    if (IsObject(nf) &&
        intl_GuardToNumberFormat(nf) === null &&
        !intl_IsWrappedNumberFormat(nf) &&
        callFunction(std_Object_isPrototypeOf, GetBuiltinPrototype("NumberFormat"), nf))
    {
        nf = nf[intlFallbackSymbol()];
    }
    return nf;
}

/**
 * Applies digit options used for number formatting onto the intl object.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.1.1.
 */
function SetNumberFormatDigitOptions(lazyData, options, mnfdDefault, mxfdDefault, notation) {
    // We skip step 1 because we set the properties on a lazyData object.

    // Steps 2-4.
    assert(IsObject(options), "SetNumberFormatDigitOptions");
    assert(typeof mnfdDefault === "number", "SetNumberFormatDigitOptions");
    assert(typeof mxfdDefault === "number", "SetNumberFormatDigitOptions");
    assert(mnfdDefault <= mxfdDefault, "SetNumberFormatDigitOptions");
    assert(typeof notation === "string", "SetNumberFormatDigitOptions");

    // Steps 5-9.
    const mnid = GetNumberOption(options, "minimumIntegerDigits", 1, 21, 1);
    let mnfd = options.minimumFractionDigits;
    let mxfd = options.maximumFractionDigits;
    let mnsd = options.minimumSignificantDigits;
    let mxsd = options.maximumSignificantDigits;

    // Step 10.
    lazyData.minimumIntegerDigits = mnid;

    // Step 11.
    if (mnsd !== undefined || mxsd !== undefined) {
        // Step 11.a (Omitted).

        // Step 11.b.
        mnsd = DefaultNumberOption(mnsd, 1, 21, 1);

        // Step 11.c.
        mxsd = DefaultNumberOption(mxsd, mnsd, 21, 21);

        // Step 11.d.
        lazyData.minimumSignificantDigits = mnsd;

        // Step 11.e.
        lazyData.maximumSignificantDigits = mxsd;
    }

    // Step 12.
    else if (mnfd !== undefined || mxfd !== undefined) {
        // Step 12.a (Omitted).

        // Step 12.b.
        mnfd = DefaultNumberOption(mnfd, 0, 20, undefined);

        // Step 12.c.
        mxfd = DefaultNumberOption(mxfd, 0, 20, undefined);

        // Steps 12.d-e.
        // Inlined DefaultNumberOption, only the fallback case applies here.
        if (mnfd === undefined) {
            assert(mxfd !== undefined, "mxfd isn't undefined when mnfd is undefined");
            mnfd = std_Math_min(mnfdDefault, mxfd);
        }

        // Step 12.f.
        // Inlined DefaultNumberOption, only the fallback case applies here.
        else if (mxfd === undefined) {
            mxfd = std_Math_max(mxfdDefault, mnfd);
        }

        // Step 12.g.
        else if (mnfd > mxfd) {
            ThrowRangeError(JSMSG_INVALID_DIGITS_VALUE, mxfd);
        }

        // Step 12.h.
        lazyData.minimumFractionDigits = mnfd;

        // Step 12.i.
        lazyData.maximumFractionDigits = mxfd;
    }

    // Step 13.
    else if (notation === "compact") {
        // Step 13.a (Omitted).
    }

    // Step 14.
    else {
        // Step 14.a (Omitted).

        // Step 14.b.
        lazyData.minimumFractionDigits = mnfdDefault;

        // Step 14.c.
        lazyData.maximumFractionDigits = mxfdDefault;
    }
}

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
        result += (0x61 <= c && c <= 0x7A)
                  ? callFunction(std_String_fromCharCode, null, c & ~0x20)
                  : s[i];
    }
    return result;
}

/**
 * Verifies that the given string is a well-formed ISO 4217 currency code.
 *
 * Spec: ECMAScript Internationalization API Specification, 6.3.1.
 */
function IsWellFormedCurrencyCode(currency) {
    assert(typeof currency === "string", "currency is a string value");

    return currency.length === 3 && IsASCIIAlphaString(currency);
}

/**
 * Verifies that the given string is a well-formed core unit identifier as
 * defined in UTS #35, Part 2, Section 6. In addition to obeying the UTS #35
 * core unit identifier syntax, |unitIdentifier| must be one of the identifiers
 * sanctioned by UTS #35 or be a compound unit composed of two sanctioned simple
 * units.
 *
 * Intl.NumberFormat Unified API Proposal
 */
function IsWellFormedUnitIdentifier(unitIdentifier) {
    assert(typeof unitIdentifier === "string", "unitIdentifier is a string value");

    // Step 1.
    if (IsSanctionedSimpleUnitIdentifier(unitIdentifier))
        return true;

    // Step 2.
    var pos = callFunction(std_String_indexOf, unitIdentifier, "-per-");
    if (pos < 0)
        return false;

    var next = pos + "-per-".length;

    // Steps 3 and 5.
    var numerator = Substring(unitIdentifier, 0, pos);
    var denominator = Substring(unitIdentifier, next, unitIdentifier.length - next);

    // Steps 4 and 6.
    return IsSanctionedSimpleUnitIdentifier(numerator) &&
           IsSanctionedSimpleUnitIdentifier(denominator);
}

#if DEBUG || MOZ_SYSTEM_ICU
var availableMeasurementUnits = {
    value: null
};
#endif

/**
 * Verifies that the given string is a sanctioned simple core unit identifier.
 *
 * Intl.NumberFormat Unified API Proposal
 *
 * Also see: https://unicode.org/reports/tr35/tr35-general.html#Unit_Elements
 */
function IsSanctionedSimpleUnitIdentifier(unitIdentifier) {
    assert(typeof unitIdentifier === "string", "unitIdentifier is a string value");

    var isSanctioned = hasOwn(unitIdentifier, sanctionedSimpleUnitIdentifiers);

#if DEBUG || MOZ_SYSTEM_ICU
    if (isSanctioned) {
        if (availableMeasurementUnits.value === null)
            availableMeasurementUnits.value = intl_availableMeasurementUnits();

        var isSupported = hasOwn(unitIdentifier, availableMeasurementUnits.value);

#if MOZ_SYSTEM_ICU
        // A system ICU may support fewer measurement units, so we need to make
        // sure the unit is actually supported.
        isSanctioned = isSupported;
#else
        // Otherwise just assert that the sanctioned unit is also supported.
        assert(isSupported,
`"${unitIdentifier}" is sanctioned but not supported. Did you forget to update
intl/icu/data_filter.json to include the unit (and any implicit compound units)?
For example "speed/kilometer-per-hour" is implied by "length/kilometer" and
"duration/hour" and must therefore also be present.`);
#endif
    }
#endif

    return isSanctioned;
}

/**
 * Initializes an object as a NumberFormat.
 *
 * This method is complicated a moderate bit by its implementing initialization
 * as a *lazy* concept.  Everything that must happen now, does -- but we defer
 * all the work we can until the object is actually used as a NumberFormat.
 * This later work occurs in |resolveNumberFormatInternals|; steps not noted
 * here occur there.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.1.2.
 */
function InitializeNumberFormat(numberFormat, thisValue, locales, options) {
    assert(IsObject(numberFormat), "InitializeNumberFormat called with non-object");
    assert(intl_GuardToNumberFormat(numberFormat) !== null,
           "InitializeNumberFormat called with non-NumberFormat");

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
    //     minimumFractionDigits: integer ∈ [0, 20],
    //     maximumFractionDigits: integer ∈ [0, 20],
    //
    //     // optional, mutually exclusive with the fraction-digits option
    //     minimumSignificantDigits: integer ∈ [1, 21],
    //     maximumSignificantDigits: integer ∈ [1, 21],
    //
    //     useGrouping: true / false,
    //
    //     notation: "standard" / "scientific" / "engineering" / "compact",
    //
    //     // optional, if notation is "compact"
    //     compactDisplay: "short" / "long",
    //
    //     signDisplay: "auto" / "never" / "always" / "exceptZero",
    //   }
    //
    // Note that lazy data is only installed as a final step of initialization,
    // so every NumberFormat lazy data object has *all* these properties, never a
    // subset of them.
    var lazyNumberFormatData = std_Object_create(null);

    // Step 1.
    var requestedLocales = CanonicalizeLocaleList(locales);
    lazyNumberFormatData.requestedLocales = requestedLocales;

    // Steps 2-3.
    //
    // If we ever need more speed here at startup, we should try to detect the
    // case where |options === undefined| and then directly use the default
    // value for each option.  For now, just keep it simple.
    if (options === undefined)
        options = std_Object_create(null);
    else
        options = ToObject(options);

    // Compute options that impact interpretation of locale.
    // Step 4.
    var opt = new_Record();
    lazyNumberFormatData.opt = opt;

    // Steps 5-6.
    var matcher = GetOption(options, "localeMatcher", "string", ["lookup", "best fit"], "best fit");
    opt.localeMatcher = matcher;

    var numberingSystem = GetOption(options, "numberingSystem", "string", undefined, undefined);

    if (numberingSystem !== undefined) {
        numberingSystem = intl_ValidateAndCanonicalizeUnicodeExtensionType(numberingSystem,
                                                                           "numberingSystem",
                                                                           "nu");
    }

    opt.nu = numberingSystem;

    // Compute formatting options.
    // Step 12.
    var style = GetOption(options, "style", "string", ["decimal", "percent", "currency", "unit"],
                          "decimal");
    lazyNumberFormatData.style = style;

    // Steps 14-17.
    var currency = GetOption(options, "currency", "string", undefined, undefined);

    // Per the Intl.NumberFormat Unified API Proposal, this check should only
    // happen for |style === "currency"|, which seems inconsistent, given that
    // we normally validate all options when present, even the ones which are
    // unused.
    // TODO: File issue at <https://github.com/tc39/proposal-unified-intl-numberformat>.
    if (currency !== undefined && !IsWellFormedCurrencyCode(currency))
        ThrowRangeError(JSMSG_INVALID_CURRENCY_CODE, currency);

    var cDigits;
    if (style === "currency") {
        if (currency === undefined)
            ThrowTypeError(JSMSG_UNDEFINED_CURRENCY);

        // Steps 19.a-c.
        currency = toASCIIUpperCase(currency);
        lazyNumberFormatData.currency = currency;
        cDigits = CurrencyDigits(currency);
    }

    // Step 18.
    var currencyDisplay = GetOption(options, "currencyDisplay", "string",
                                    ["code", "symbol", "narrowSymbol", "name"], "symbol");
    if (style === "currency")
        lazyNumberFormatData.currencyDisplay = currencyDisplay;

    // Intl.NumberFormat Unified API Proposal
    var currencySign = GetOption(options, "currencySign", "string", ["standard", "accounting"],
                                 "standard");
    if (style === "currency")
        lazyNumberFormatData.currencySign = currencySign;

    // Intl.NumberFormat Unified API Proposal
    var unit = GetOption(options, "unit", "string", undefined, undefined);

    // Aligned with |currency| check from above, see note about spec issue there.
    if (unit !== undefined && !IsWellFormedUnitIdentifier(unit))
        ThrowRangeError(JSMSG_INVALID_UNIT_IDENTIFIER, unit);

    var unitDisplay = GetOption(options, "unitDisplay", "string",
                                ["short", "narrow", "long"], "short");

    if (style === "unit") {
        if (unit === undefined)
            ThrowTypeError(JSMSG_UNDEFINED_UNIT);

        lazyNumberFormatData.unit = unit;
        lazyNumberFormatData.unitDisplay = unitDisplay;
    }

    // Steps 20-21.
    var mnfdDefault, mxfdDefault;
    if (style === "currency") {
        mnfdDefault = cDigits;
        mxfdDefault = cDigits;
    } else {
        mnfdDefault = 0;
        mxfdDefault = style === "percent" ? 0 : 3;
    }

    // Intl.NumberFormat Unified API Proposal
    var notation = GetOption(options, "notation", "string",
                             ["standard", "scientific", "engineering", "compact"], "standard");
    lazyNumberFormatData.notation = notation;

    // Step 22.
    SetNumberFormatDigitOptions(lazyNumberFormatData, options, mnfdDefault, mxfdDefault, notation);

    // Intl.NumberFormat Unified API Proposal
    var compactDisplay = GetOption(options, "compactDisplay", "string",
                                   ["short", "long"], "short");
    if (notation === "compact")
        lazyNumberFormatData.compactDisplay = compactDisplay;

    // Steps 23.
    var useGrouping = GetOption(options, "useGrouping", "boolean", undefined, true);
    lazyNumberFormatData.useGrouping = useGrouping;

    // Intl.NumberFormat Unified API Proposal
    var signDisplay = GetOption(options, "signDisplay", "string",
                                ["auto", "never", "always", "exceptZero"], "auto");
    lazyNumberFormatData.signDisplay = signDisplay;

    // Step 31.
    //
    // We've done everything that must be done now: mark the lazy data as fully
    // computed and install it.
    initializeIntlObject(numberFormat, "NumberFormat", lazyNumberFormatData);

    // 11.2.1, steps 4-5.
    if (numberFormat !== thisValue &&
        callFunction(std_Object_isPrototypeOf, GetBuiltinPrototype("NumberFormat"), thisValue))
    {
        DefineDataProperty(thisValue, intlFallbackSymbol(), numberFormat,
                           ATTR_NONENUMERABLE | ATTR_NONCONFIGURABLE | ATTR_NONWRITABLE);

        return thisValue;
    }

    // 11.2.1, step 6.
    return numberFormat;
}

/**
 * Returns the number of decimal digits to be used for the given currency.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.1.3.
 */
function CurrencyDigits(currency) {
    assert(typeof currency === "string", "currency is a string value");
    assert(IsWellFormedCurrencyCode(currency), "currency is well-formed");
    assert(currency == toASCIIUpperCase(currency), "currency is all upper-case");

    if (hasOwn(currency, currencyDigits))
        return currencyDigits[currency];
    return 2;
}

/**
 * Returns the subset of the given locale list for which this locale list has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.3.2.
 */
function Intl_NumberFormat_supportedLocalesOf(locales /*, options*/) {
    var options = arguments.length > 1 ? arguments[1] : undefined;

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
    return [
        defaultNumberingSystem,
        NUMBERING_SYSTEMS_WITH_SIMPLE_DIGIT_MAPPINGS
    ];
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
 * Create function to be cached and returned by Intl.NumberFormat.prototype.format.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.1.4.
 */
function createNumberFormatFormat(nf) {
    // This function is not inlined in $Intl_NumberFormat_format_get to avoid
    // creating a call-object on each call to $Intl_NumberFormat_format_get.
    return function(value) {
        // Step 1 (implicit).

        // Step 2.
        assert(IsObject(nf), "InitializeNumberFormat called with non-object");
        assert(intl_GuardToNumberFormat(nf) !== null,
               "InitializeNumberFormat called with non-NumberFormat");

        // Steps 3-4.
        var x = ToNumeric(value);

        // Step 5.
        return intl_FormatNumber(nf, x, /* formatToParts = */ false);
    };
}

/**
 * Returns a function bound to this NumberFormat that returns a String value
 * representing the result of calling ToNumber(value) according to the
 * effective locale and the formatting options of this NumberFormat.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.4.3.
 */
// Uncloned functions with `$` prefix are allocated as extended function
// to store the original name in `SetCanonicalName`.
function $Intl_NumberFormat_format_get() {
    // Steps 1-3.
    var thisArg = UnwrapNumberFormat(this);
    var nf = thisArg;
    if (!IsObject(nf) || (nf = intl_GuardToNumberFormat(nf)) === null) {
        return callFunction(intl_CallNumberFormatMethodIfWrapped, thisArg,
                            "$Intl_NumberFormat_format_get");
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
 * 11.4.4 Intl.NumberFormat.prototype.formatToParts ( value )
 */
function Intl_NumberFormat_formatToParts(value) {
    // Step 1.
    var nf = this;

    // Steps 2-3.
    if (!IsObject(nf) || (nf = intl_GuardToNumberFormat(nf)) === null) {
        return callFunction(intl_CallNumberFormatMethodIfWrapped, this, value,
                            "Intl_NumberFormat_formatToParts");
    }

    // Step 4.
    var x = ToNumeric(value);

    // Step 5.
    return intl_FormatNumber(nf, x, /* formatToParts = */ true);
}

/**
 * Returns the resolved options for a NumberFormat object.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.4.5.
 */
function Intl_NumberFormat_resolvedOptions() {
    // Steps 1-3.
    var thisArg = UnwrapNumberFormat(this);
    var nf = thisArg;
    if (!IsObject(nf) || (nf = intl_GuardToNumberFormat(nf)) === null) {
        return callFunction(intl_CallNumberFormatMethodIfWrapped, thisArg,
                            "Intl_NumberFormat_resolvedOptions");
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
    assert(hasOwn("currency", internals) === (internals.style === "currency"),
           "currency is present iff style is 'currency'");
    assert(hasOwn("currencyDisplay", internals) === (internals.style === "currency"),
           "currencyDisplay is present iff style is 'currency'");
    assert(hasOwn("currencySign", internals) === (internals.style === "currency"),
           "currencySign is present iff style is 'currency'");

    if (hasOwn("currency", internals)) {
        DefineDataProperty(result, "currency", internals.currency);
        DefineDataProperty(result, "currencyDisplay", internals.currencyDisplay);
        DefineDataProperty(result, "currencySign", internals.currencySign);
    }

    // unit and unitDisplay are only present for unit formatters.
    assert(hasOwn("unit", internals) === (internals.style === "unit"),
           "unit is present iff style is 'unit'");
    assert(hasOwn("unitDisplay", internals) === (internals.style === "unit"),
           "unitDisplay is present iff style is 'unit'");

    if (hasOwn("unit", internals)) {
        DefineDataProperty(result, "unit", internals.unit);
        DefineDataProperty(result, "unitDisplay", internals.unitDisplay);
    }

    DefineDataProperty(result, "minimumIntegerDigits", internals.minimumIntegerDigits);

    // Min/Max fraction digits are either both present or not present at all.
    assert(hasOwn("minimumFractionDigits", internals) ===
           hasOwn("maximumFractionDigits", internals),
           "minimumFractionDigits is present iff maximumFractionDigits is present");

    if (hasOwn("minimumFractionDigits", internals)) {
        DefineDataProperty(result, "minimumFractionDigits", internals.minimumFractionDigits);
        DefineDataProperty(result, "maximumFractionDigits", internals.maximumFractionDigits);
    }

    // Min/Max significant digits are either both present or not present at all.
    assert(hasOwn("minimumSignificantDigits", internals) ===
           hasOwn("maximumSignificantDigits", internals),
           "minimumSignificantDigits is present iff maximumSignificantDigits is present");

    if (hasOwn("minimumSignificantDigits", internals)) {
        DefineDataProperty(result, "minimumSignificantDigits",
                           internals.minimumSignificantDigits);
        DefineDataProperty(result, "maximumSignificantDigits",
                           internals.maximumSignificantDigits);
    }

    DefineDataProperty(result, "useGrouping", internals.useGrouping);

    var notation = internals.notation;
    DefineDataProperty(result, "notation", notation);

    if (notation === "compact")
        DefineDataProperty(result, "compactDisplay", internals.compactDisplay);

    DefineDataProperty(result, "signDisplay", internals.signDisplay);

    // Step 6.
    return result;
}
