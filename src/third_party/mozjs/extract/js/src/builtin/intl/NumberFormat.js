/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Portions Copyright Norbert Lindenberg 2011-2012. */

/**
 * NumberFormat internal properties.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.1 and 11.3.3.
 */
var numberFormatInternalProperties = {
    localeData: numberFormatLocaleData,
    _availableLocales: null,
    availableLocales: function() // eslint-disable-line object-shorthand
    {
        var locales = this._availableLocales;
        if (locales)
            return locales;

        locales = intl_NumberFormat_availableLocales();
        addSpecialMissingLanguageTags(locales);
        return (this._availableLocales = locales);
    },
    relevantExtensionKeys: ["nu"]
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
    var r = ResolveLocale(callFunction(NumberFormat.availableLocales, NumberFormat),
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
    }

    // Step 22.
    internalProps.minimumIntegerDigits = lazyNumberFormatData.minimumIntegerDigits;
    internalProps.minimumFractionDigits = lazyNumberFormatData.minimumFractionDigits;
    internalProps.maximumFractionDigits = lazyNumberFormatData.maximumFractionDigits;

    if ("minimumSignificantDigits" in lazyNumberFormatData) {
        // Note: Intl.NumberFormat.prototype.resolvedOptions() exposes the
        // actual presence (versus undefined-ness) of these properties.
        assert("maximumSignificantDigits" in lazyNumberFormatData, "min/max sig digits mismatch");
        internalProps.minimumSignificantDigits = lazyNumberFormatData.minimumSignificantDigits;
        internalProps.maximumSignificantDigits = lazyNumberFormatData.maximumSignificantDigits;
    }

    // Step 24.
    internalProps.useGrouping = lazyNumberFormatData.useGrouping;

    // The caller is responsible for associating |internalProps| with the right
    // object using |setInternalProperties|.
    return internalProps;
}

/**
 * Returns an object containing the NumberFormat internal properties of |obj|.
 */
function getNumberFormatInternals(obj) {
    assert(IsObject(obj), "getNumberFormatInternals called with non-object");
    assert(GuardToNumberFormat(obj) !== null, "getNumberFormatInternals called with non-NumberFormat");

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
function UnwrapNumberFormat(nf, methodName) {
    // Step 1 (not applicable in our implementation).

    // Step 2.
    if (IsObject(nf) && (GuardToNumberFormat(nf)) === null && nf instanceof GetNumberFormatConstructor())
        nf = nf[intlFallbackSymbol()];

    // Step 3.
    if (!IsObject(nf) || (nf = GuardToNumberFormat(nf)) === null)
        ThrowTypeError(JSMSG_INTL_OBJECT_NOT_INITED, "NumberFormat", methodName, "NumberFormat");

    // Step 4.
    return nf;
}

/**
 * Applies digit options used for number formatting onto the intl object.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.1.1.
 */
function SetNumberFormatDigitOptions(lazyData, options, mnfdDefault, mxfdDefault) {
    // We skip step 1 because we set the properties on a lazyData object.

    // Steps 2-4.
    assert(IsObject(options), "SetNumberFormatDigitOptions");
    assert(typeof mnfdDefault === "number", "SetNumberFormatDigitOptions");
    assert(typeof mxfdDefault === "number", "SetNumberFormatDigitOptions");
    assert(mnfdDefault <= mxfdDefault, "SetNumberFormatDigitOptions");

    // Steps 5-8.
    const mnid = GetNumberOption(options, "minimumIntegerDigits", 1, 21, 1);
    const mnfd = GetNumberOption(options, "minimumFractionDigits", 0, 20, mnfdDefault);
    const mxfdActualDefault = std_Math_max(mnfd, mxfdDefault);
    const mxfd = GetNumberOption(options, "maximumFractionDigits", mnfd, 20, mxfdActualDefault);

    // Steps 9-10.
    let mnsd = options.minimumSignificantDigits;
    let mxsd = options.maximumSignificantDigits;

    // Steps 11-13.
    lazyData.minimumIntegerDigits = mnid;
    lazyData.minimumFractionDigits = mnfd;
    lazyData.maximumFractionDigits = mxfd;

    // Step 14.
    if (mnsd !== undefined || mxsd !== undefined) {
        mnsd = DefaultNumberOption(mnsd, 1, 21, 1);
        mxsd = DefaultNumberOption(mxsd, mnsd, 21, 21);
        lazyData.minimumSignificantDigits = mnsd;
        lazyData.maximumSignificantDigits = mxsd;
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
    assert(GuardToNumberFormat(numberFormat) !== null, "InitializeNumberFormat called with non-NumberFormat");

    // Lazy NumberFormat data has the following structure:
    //
    //   {
    //     requestedLocales: List of locales,
    //     style: "decimal" / "percent" / "currency",
    //
    //     // fields present only if style === "currency":
    //     currency: a well-formed currency code (IsWellFormedCurrencyCode),
    //     currencyDisplay: "code" / "symbol" / "name",
    //
    //     opt: // opt object computed in InitializeNumberFormat
    //       {
    //         localeMatcher: "lookup" / "best fit",
    //       }
    //
    //     minimumIntegerDigits: integer ∈ [1, 21],
    //     minimumFractionDigits: integer ∈ [0, 20],
    //     maximumFractionDigits: integer ∈ [0, 20],
    //
    //     // optional
    //     minimumSignificantDigits: integer ∈ [1, 21],
    //     maximumSignificantDigits: integer ∈ [1, 21],
    //
    //     useGrouping: true / false,
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
    var opt = new Record();
    lazyNumberFormatData.opt = opt;

    // Steps 5-6.
    var matcher = GetOption(options, "localeMatcher", "string", ["lookup", "best fit"], "best fit");
    opt.localeMatcher = matcher;

    // Compute formatting options.
    // Step 12.
    var style = GetOption(options, "style", "string", ["decimal", "percent", "currency"], "decimal");
    lazyNumberFormatData.style = style;

    // Steps 14-17.
    var c = GetOption(options, "currency", "string", undefined, undefined);
    if (c !== undefined && !IsWellFormedCurrencyCode(c))
        ThrowRangeError(JSMSG_INVALID_CURRENCY_CODE, c);
    var cDigits;
    if (style === "currency") {
        if (c === undefined)
            ThrowTypeError(JSMSG_UNDEFINED_CURRENCY);

        // Steps 19.a-c.
        c = toASCIIUpperCase(c);
        lazyNumberFormatData.currency = c;
        cDigits = CurrencyDigits(c);
    }

    // Step 18.
    var cd = GetOption(options, "currencyDisplay", "string", ["code", "symbol", "name"], "symbol");
    if (style === "currency")
        lazyNumberFormatData.currencyDisplay = cd;

    // Steps 20-22.
    var mnfdDefault, mxfdDefault;
    if (style === "currency") {
        mnfdDefault = cDigits;
        mxfdDefault = cDigits;
    } else {
        mnfdDefault = 0;
        mxfdDefault = style === "percent" ? 0 : 3;
    }
    SetNumberFormatDigitOptions(lazyNumberFormatData, options, mnfdDefault, mxfdDefault);

    // Steps 23.
    var g = GetOption(options, "useGrouping", "boolean", undefined, true);
    lazyNumberFormatData.useGrouping = g;

    // Step 31.
    //
    // We've done everything that must be done now: mark the lazy data as fully
    // computed and install it.
    initializeIntlObject(numberFormat, "NumberFormat", lazyNumberFormatData);

    // 11.2.1, steps 4-5.
    // TODO: spec issue - The current spec doesn't have the IsObject check,
    // which means |Intl.NumberFormat.call(null)| is supposed to throw here.
    if (numberFormat !== thisValue && IsObject(thisValue) &&
        thisValue instanceof GetNumberFormatConstructor())
    {
        _DefineDataProperty(thisValue, intlFallbackSymbol(), numberFormat,
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
    var availableLocales = callFunction(numberFormatInternalProperties.availableLocales,
                                        numberFormatInternalProperties);

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
    // lack of information we don't offer them. To increase chances that
    // other software will process output correctly, we further restrict to
    // those decimal numbering systems explicitly listed in table 3 of
    // the ECMAScript Internationalization API Specification, 11.1.6, which
    // in turn are those with full specifications in version 21 of Unicode
    // Technical Standard #35 using digits that were defined in Unicode 5.0,
    // the Unicode version supported in Windows Vista.
    // The one thing we can find out from ICU is the default numbering system
    // for a locale.
    var defaultNumberingSystem = intl_numberingSystem(locale);
    return [
        defaultNumberingSystem,
        "arab", "arabext", "bali", "beng", "deva",
        "fullwide", "gujr", "guru", "hanidec", "khmr",
        "knda", "laoo", "latn", "limb", "mlym",
        "mong", "mymr", "orya", "tamldec", "telu",
        "thai", "tibt"
    ];
}

function numberFormatLocaleData() {
    return {
        nu: getNumberingSystems,
        default: {
            nu: intl_numberingSystem,
        }
    };
}

/**
 * Function to be bound and returned by Intl.NumberFormat.prototype.format.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.1.4.
 */
function numberFormatFormatToBind(value) {
    // Step 1.
    var nf = this;

    // Step 2.
    assert(IsObject(nf), "InitializeNumberFormat called with non-object");
    assert(GuardToNumberFormat(nf) !== null, "InitializeNumberFormat called with non-NumberFormat");

    // Steps 3-4.
    var x = ToNumber(value);

    // Step 5.
    return intl_FormatNumber(nf, x, /* formatToParts = */ false);
}

/**
 * Returns a function bound to this NumberFormat that returns a String value
 * representing the result of calling ToNumber(value) according to the
 * effective locale and the formatting options of this NumberFormat.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.4.3.
 */
function Intl_NumberFormat_format_get() {
    // Steps 1-3.
    var nf = UnwrapNumberFormat(this, "format");

    var internals = getNumberFormatInternals(nf);

    // Step 4.
    if (internals.boundFormat === undefined) {
        // Steps 4.a-b.
        var F = callFunction(FunctionBind, numberFormatFormatToBind, nf);

        // Step 4.c.
        internals.boundFormat = F;
    }

    // Step 5.
    return internals.boundFormat;
}
_SetCanonicalName(Intl_NumberFormat_format_get, "get format");

/**
 * 11.4.4 Intl.NumberFormat.prototype.formatToParts ( value )
 */
function Intl_NumberFormat_formatToParts(value) {
    // Step 1.
    var nf = this;

    // Steps 2-3.
    if (!IsObject(nf) || (nf = GuardToNumberFormat(nf)) === null) {
        ThrowTypeError(JSMSG_INTL_OBJECT_NOT_INITED, "NumberFormat", "formatToParts",
                       "NumberFormat");
    }

    // Ensure the NumberFormat internals are resolved.
    getNumberFormatInternals(nf);

    // Step 4.
    var x = ToNumber(value);

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
    var nf = UnwrapNumberFormat(this, "resolvedOptions");

    var internals = getNumberFormatInternals(nf);

    // Steps 4-5.
    var result = {
        locale: internals.locale,
        numberingSystem: internals.numberingSystem,
        style: internals.style,
        minimumIntegerDigits: internals.minimumIntegerDigits,
        minimumFractionDigits: internals.minimumFractionDigits,
        maximumFractionDigits: internals.maximumFractionDigits,
        useGrouping: internals.useGrouping
    };

    // currency and currencyDisplay are only present for currency formatters.
    assert(hasOwn("currency", internals) === (internals.style === "currency"),
           "currency is present iff style is 'currency'");
    assert(hasOwn("currencyDisplay", internals) === (internals.style === "currency"),
           "currencyDisplay is present iff style is 'currency'");

    if (hasOwn("currency", internals)) {
        _DefineDataProperty(result, "currency", internals.currency);
        _DefineDataProperty(result, "currencyDisplay", internals.currencyDisplay);
    }

    // Min/Max significant digits are either both present or not at all.
    assert(hasOwn("minimumSignificantDigits", internals) ===
           hasOwn("maximumSignificantDigits", internals),
           "minimumSignificantDigits is present iff maximumSignificantDigits is present");

    if (hasOwn("minimumSignificantDigits", internals)) {
        _DefineDataProperty(result, "minimumSignificantDigits",
                            internals.minimumSignificantDigits);
        _DefineDataProperty(result, "maximumSignificantDigits",
                            internals.maximumSignificantDigits);
    }

    // Step 6.
    return result;
}
