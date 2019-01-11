/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Portions Copyright Norbert Lindenberg 2011-2012. */

/**
 * Compute an internal properties object from |lazyCollatorData|.
 */
function resolveCollatorInternals(lazyCollatorData) {
    assert(IsObject(lazyCollatorData), "lazy data not an object?");

    var internalProps = std_Object_create(null);

    var Collator = collatorInternalProperties;

    // Step 5.
    internalProps.usage = lazyCollatorData.usage;

    // Steps 6-7.
    var collatorIsSorting = lazyCollatorData.usage === "sort";
    var localeData = collatorIsSorting
                     ? Collator.sortLocaleData
                     : Collator.searchLocaleData;

    // Compute effective locale.
    // Step 16.
    var relevantExtensionKeys = Collator.relevantExtensionKeys;

    // Step 17.
    var r = ResolveLocale(callFunction(Collator.availableLocales, Collator),
                          lazyCollatorData.requestedLocales,
                          lazyCollatorData.opt,
                          relevantExtensionKeys,
                          localeData);

    // Step 18.
    internalProps.locale = r.locale;

    // Step 19.
    var collation = r.co;

    // Step 20.
    if (collation === null)
        collation = "default";

    // Step 21.
    internalProps.collation = collation;

    // Step 22.
    internalProps.numeric = r.kn === "true";

    // Step 23.
    internalProps.caseFirst = r.kf;

    // Compute remaining collation options.
    // Step 25.
    var s = lazyCollatorData.rawSensitivity;
    if (s === undefined) {
        // In theory the default sensitivity for the "search" collator is
        // locale dependent; in reality the CLDR/ICU default strength is
        // always tertiary. Therefore use "variant" as the default value for
        // both collation modes.
        s = "variant";
    }

    // Step 26.
    internalProps.sensitivity = s;

    // Step 28.
    internalProps.ignorePunctuation = lazyCollatorData.ignorePunctuation;

    // The caller is responsible for associating |internalProps| with the right
    // object using |setInternalProperties|.
    return internalProps;
}

/**
 * Returns an object containing the Collator internal properties of |obj|.
 */
function getCollatorInternals(obj) {
    assert(IsObject(obj), "getCollatorInternals called with non-object");
    assert(GuardToCollator(obj) !== null, "getCollatorInternals called with non-Collator");

    var internals = getIntlObjectInternals(obj);
    assert(internals.type === "Collator", "bad type escaped getIntlObjectInternals");

    // If internal properties have already been computed, use them.
    var internalProps = maybeInternalProperties(internals);
    if (internalProps)
        return internalProps;

    // Otherwise it's time to fully create them.
    internalProps = resolveCollatorInternals(internals.lazyData);
    setInternalProperties(internals, internalProps);
    return internalProps;
}

/**
 * Initializes an object as a Collator.
 *
 * This method is complicated a moderate bit by its implementing initialization
 * as a *lazy* concept.  Everything that must happen now, does -- but we defer
 * all the work we can until the object is actually used as a Collator.  This
 * later work occurs in |resolveCollatorInternals|; steps not noted here occur
 * there.
 *
 * Spec: ECMAScript Internationalization API Specification, 10.1.1.
 */
function InitializeCollator(collator, locales, options) {
    assert(IsObject(collator), "InitializeCollator called with non-object");
    assert(GuardToCollator(collator) != null, "InitializeCollator called with non-Collator");

    // Lazy Collator data has the following structure:
    //
    //   {
    //     requestedLocales: List of locales,
    //     usage: "sort" / "search",
    //     opt: // opt object computed in InitializeCollator
    //       {
    //         localeMatcher: "lookup" / "best fit",
    //         kn: true / false / undefined,
    //         kf: "upper" / "lower" / "false" / undefined
    //       }
    //     rawSensitivity: "base" / "accent" / "case" / "variant" / undefined,
    //     ignorePunctuation: true / false
    //   }
    //
    // Note that lazy data is only installed as a final step of initialization,
    // so every Collator lazy data object has *all* these properties, never a
    // subset of them.
    var lazyCollatorData = std_Object_create(null);

    // Step 1.
    var requestedLocales = CanonicalizeLocaleList(locales);
    lazyCollatorData.requestedLocales = requestedLocales;

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
    var u = GetOption(options, "usage", "string", ["sort", "search"], "sort");
    lazyCollatorData.usage = u;

    // Step 8.
    var opt = new Record();
    lazyCollatorData.opt = opt;

    // Steps 9-10.
    var matcher = GetOption(options, "localeMatcher", "string", ["lookup", "best fit"], "best fit");
    opt.localeMatcher = matcher;

    // Steps 11-13.
    var numericValue = GetOption(options, "numeric", "boolean", undefined, undefined);
    if (numericValue !== undefined)
        numericValue = numericValue ? "true" : "false";
    opt.kn = numericValue;

    // Steps 14-15.
    var caseFirstValue = GetOption(options, "caseFirst", "string", ["upper", "lower", "false"], undefined);
    opt.kf = caseFirstValue;

    // Compute remaining collation options.
    // Step 24.
    var s = GetOption(options, "sensitivity", "string",
                      ["base", "accent", "case", "variant"], undefined);
    lazyCollatorData.rawSensitivity = s;

    // Step 27.
    var ip = GetOption(options, "ignorePunctuation", "boolean", undefined, false);
    lazyCollatorData.ignorePunctuation = ip;

    // Step 29.
    //
    // We've done everything that must be done now: mark the lazy data as fully
    // computed and install it.
    initializeIntlObject(collator, "Collator", lazyCollatorData);
}

/**
 * Returns the subset of the given locale list for which this locale list has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 *
 * Spec: ECMAScript Internationalization API Specification, 10.2.2.
 */
function Intl_Collator_supportedLocalesOf(locales /*, options*/) {
    var options = arguments.length > 1 ? arguments[1] : undefined;

    // Step 1.
    var availableLocales = callFunction(collatorInternalProperties.availableLocales,
                                        collatorInternalProperties);

    // Step 2.
    var requestedLocales = CanonicalizeLocaleList(locales);

    // Step 3.
    return SupportedLocales(availableLocales, requestedLocales, options);
}

/**
 * Collator internal properties.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.1 and 10.2.3.
 */
var collatorInternalProperties = {
    sortLocaleData: collatorSortLocaleData,
    searchLocaleData: collatorSearchLocaleData,
    _availableLocales: null,
    availableLocales: function() // eslint-disable-line object-shorthand
    {
        var locales = this._availableLocales;
        if (locales)
            return locales;

        locales = intl_Collator_availableLocales();
        addSpecialMissingLanguageTags(locales);
        return (this._availableLocales = locales);
    },
    relevantExtensionKeys: ["co", "kn", "kf"]
};

/**
 * Returns the actual locale used when a collator for |locale| is constructed.
 */
function collatorActualLocale(locale) {
    assert(typeof locale === "string", "locale should be string");

    // If |locale| is the default locale (e.g. da-DK), but only supported
    // through a fallback (da), we need to get the actual locale before we
    // can call intl_isUpperCaseFirst. Also see BestAvailableLocaleHelper.
    var availableLocales = callFunction(collatorInternalProperties.availableLocales,
                                        collatorInternalProperties);
    return BestAvailableLocaleIgnoringDefault(availableLocales, locale);
}

/**
 * Returns the default caseFirst values for the given locale. The first
 * element in the returned array denotes the default value per ES2017 Intl,
 * 9.1 Internal slots of Service Constructors.
 */
function collatorSortCaseFirst(locale) {
    var actualLocale = collatorActualLocale(locale);
    if (intl_isUpperCaseFirst(actualLocale))
        return ["upper", "false", "lower"];

    // Default caseFirst values for all other languages.
    return ["false", "lower", "upper"];
}

/**
 * Returns the default caseFirst value for the given locale.
 */
function collatorSortCaseFirstDefault(locale) {
    var actualLocale = collatorActualLocale(locale);
    if (intl_isUpperCaseFirst(actualLocale))
        return "upper";

    // Default caseFirst value for all other languages.
    return "false";
}

function collatorSortLocaleData() {
    /* eslint-disable object-shorthand */
    return {
        co: intl_availableCollations,
        kn: function() {
            return ["false", "true"];
        },
        kf: collatorSortCaseFirst,
        default: {
            co: function() {
                // The first element of the collations array must be |null|
                // per ES2017 Intl, 10.2.3 Internal Slots.
                return null;
            },
            kn: function() {
                return "false";
            },
            kf: collatorSortCaseFirstDefault,
        }
    };
    /* eslint-enable object-shorthand */
}

function collatorSearchLocaleData() {
    /* eslint-disable object-shorthand */
    return {
        co: function() {
            return [null];
        },
        kn: function() {
            return ["false", "true"];
        },
        kf: function() {
            return ["false", "lower", "upper"];
        },
        default: {
            co: function() {
                return null;
            },
            kn: function() {
                return "false";
            },
            kf: function() {
                return "false";
            },
        }
    };
    /* eslint-enable object-shorthand */
}

/**
 * Function to be bound and returned by Intl.Collator.prototype.compare.
 *
 * Spec: ECMAScript Internationalization API Specification, 10.3.3.1.
 */
function collatorCompareToBind(x, y) {
    // Step 1.
    var collator = this;

    // Step 2.
    assert(IsObject(collator), "collatorCompareToBind called with non-object");
    assert(GuardToCollator(collator) !== null, "collatorCompareToBind called with non-Collator");

    // Steps 3-6
    var X = ToString(x);
    var Y = ToString(y);

    // Step 7.
    return intl_CompareStrings(collator, X, Y);
}

/**
 * Returns a function bound to this Collator that compares x (converted to a
 * String value) and y (converted to a String value),
 * and returns a number less than 0 if x < y, 0 if x = y, or a number greater
 * than 0 if x > y according to the sort order for the locale and collation
 * options of this Collator object.
 *
 * Spec: ECMAScript Internationalization API Specification, 10.3.3.
 */
function Intl_Collator_compare_get() {
    // Step 1.
    var collator = this;

    // Steps 2-3.
    if (!IsObject(collator) || (collator = GuardToCollator(collator)) === null)
        ThrowTypeError(JSMSG_INTL_OBJECT_NOT_INITED, "Collator", "compare", "Collator");

    var internals = getCollatorInternals(collator);

    // Step 4.
    if (internals.boundCompare === undefined) {
        // Steps 4.a-b.
        var F = callFunction(FunctionBind, collatorCompareToBind, collator);

        // Step 4.c.
        internals.boundCompare = F;
    }

    // Step 5.
    return internals.boundCompare;
}
_SetCanonicalName(Intl_Collator_compare_get, "get compare");

/**
 * Returns the resolved options for a Collator object.
 *
 * Spec: ECMAScript Internationalization API Specification, 10.3.4.
 */
function Intl_Collator_resolvedOptions() {
    // Step 1.
    var collator = this;

    // Steps 2-3.
    if (!IsObject(collator) || (collator = GuardToCollator(collator)) === null)
        ThrowTypeError(JSMSG_INTL_OBJECT_NOT_INITED, "Collator", "resolvedOptions", "Collator");

    var internals = getCollatorInternals(collator);

    // Steps 4-5.
    var result = {
        locale: internals.locale,
        usage: internals.usage,
        sensitivity: internals.sensitivity,
        ignorePunctuation: internals.ignorePunctuation,
        collation: internals.collation,
        numeric: internals.numeric,
        caseFirst: internals.caseFirst,
    };

    // Step 6.
    return result;
}
