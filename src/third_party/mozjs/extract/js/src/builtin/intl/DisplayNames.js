/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Intl.DisplayNames internal properties.
 */
function displayNamesLocaleData() {
    // Intl.DisplayNames doesn't support any extension keys.
    return {};
}
var displayNamesInternalProperties = {
    localeData: displayNamesLocaleData,
    relevantExtensionKeys: []
};

function mozDisplayNamesLocaleData() {
    return {
        ca: intl_availableCalendars,
        default: {
            ca: intl_defaultCalendar,
        },
    };
}
var mozDisplayNamesInternalProperties = {
    localeData: mozDisplayNamesLocaleData,
    relevantExtensionKeys: ["ca"]
};

/**
 * Intl.DisplayNames ( [ locales [ , options ] ] )
 *
 * Compute an internal properties object from |lazyDisplayNamesData|.
 */
function resolveDisplayNamesInternals(lazyDisplayNamesData) {
    assert(IsObject(lazyDisplayNamesData), "lazy data not an object?");

    var internalProps = std_Object_create(null);

    var mozExtensions = lazyDisplayNamesData.mozExtensions;

    var DisplayNames = mozExtensions ?
                       mozDisplayNamesInternalProperties :
                       displayNamesInternalProperties;

    // Compute effective locale.

    // Step 7.
    var localeData = DisplayNames.localeData;

    // Step 10.
    var r = ResolveLocale("DisplayNames",
                          lazyDisplayNamesData.requestedLocales,
                          lazyDisplayNamesData.opt,
                          DisplayNames.relevantExtensionKeys,
                          localeData);
    // Step 12.
    internalProps.style = lazyDisplayNamesData.style;

    // Step 14.
    var type = lazyDisplayNamesData.type;
    internalProps.type = type;

    // Step 16.
    internalProps.fallback = lazyDisplayNamesData.fallback;

    // Step 17.
    internalProps.locale = r.locale;

    // Step 25.
    if (type === "language") {
        internalProps.languageDisplay = lazyDisplayNamesData.languageDisplay;
    }

    if (mozExtensions) {
        internalProps.calendar = r.ca;
    }

    // The caller is responsible for associating |internalProps| with the right
    // object using |setInternalProperties|.
    return internalProps;
}

/**
 * Returns an object containing the DisplayNames internal properties of |obj|.
 */
function getDisplayNamesInternals(obj) {
    assert(IsObject(obj), "getDisplayNamesInternals called with non-object");
    assert(intl_GuardToDisplayNames(obj) !== null,
           "getDisplayNamesInternals called with non-DisplayNames");

    var internals = getIntlObjectInternals(obj);
    assert(internals.type === "DisplayNames", "bad type escaped getIntlObjectInternals");

    // If internal properties have already been computed, use them.
    var internalProps = maybeInternalProperties(internals);
    if (internalProps)
        return internalProps;

    // Otherwise it's time to fully create them.
    internalProps = resolveDisplayNamesInternals(internals.lazyData);
    setInternalProperties(internals, internalProps);
    return internalProps;
}

/**
 * Intl.DisplayNames ( [ locales [ , options ] ] )
 *
 * Initializes an object as a DisplayNames.
 *
 * This method is complicated a moderate bit by its implementing initialization
 * as a *lazy* concept.  Everything that must happen now, does -- but we defer
 * all the work we can until the object is actually used as a DisplayNames.
 * This later work occurs in |resolveDisplayNamesInternals|; steps not noted
 * here occur there.
 */
function InitializeDisplayNames(displayNames, locales, options, mozExtensions) {
    assert(IsObject(displayNames), "InitializeDisplayNames called with non-object");
    assert(intl_GuardToDisplayNames(displayNames) !== null,
           "InitializeDisplayNames called with non-DisplayNames");

    // Lazy DisplayNames data has the following structure:
    //
    //   {
    //     requestedLocales: List of locales,
    //
    //     opt: // opt object computed in InitializeDisplayNames
    //       {
    //         localeMatcher: "lookup" / "best fit",
    //
    //         ca: string matching a Unicode extension type, // optional
    //       }
    //
    //     localeMatcher: "lookup" / "best fit",
    //
    //     style: "narrow" / "short" / "long",
    //
    //     type: "language" / "region" / "script" / "currency" / "weekday" /
    //           "month" / "quarter" / "dayPeriod" / "dateTimeField"
    //
    //     fallback: "code" / "none",
    //
    //     // field present only if type === "language":
    //     languageDisplay: "dialect" / "standard",
    //
    //     mozExtensions: true / false,
    //   }
    //
    // Note that lazy data is only installed as a final step of initialization,
    // so every DisplayNames lazy data object has *all* these properties, never a
    // subset of them.
    var lazyDisplayNamesData = std_Object_create(null);

    // Step 3.
    var requestedLocales = CanonicalizeLocaleList(locales);
    lazyDisplayNamesData.requestedLocales = requestedLocales;

    // Step 4.
    if (!IsObject(options))
        ThrowTypeError(JSMSG_OBJECT_REQUIRED, options === null ? "null" : typeof options);

    // Step 5.
    var opt = new_Record();
    lazyDisplayNamesData.opt = opt;
    lazyDisplayNamesData.mozExtensions = mozExtensions;

    // Steps 7-8.
    var matcher = GetOption(options, "localeMatcher", "string", ["lookup", "best fit"], "best fit");
    opt.localeMatcher = matcher;

    if (mozExtensions) {
        var calendar = GetOption(options, "calendar", "string", undefined, undefined);

        if (calendar !== undefined) {
            calendar = intl_ValidateAndCanonicalizeUnicodeExtensionType(calendar, "calendar", "ca");
        }

        opt.ca = calendar;
    }

    // Step 10.
    var style = GetOption(options, "style", "string", ["narrow", "short", "long"], "long");

    // Step 11.
    lazyDisplayNamesData.style = style;

    // Step 12.
    var type;
    if (mozExtensions) {
        type = GetOption(options, "type", "string",
                         ["language", "region", "script", "currency", "calendar", "dateTimeField",
                          "weekday", "month", "quarter", "dayPeriod"],
                          undefined);
    } else {
        type = GetOption(options, "type", "string",
                         ["language", "region", "script", "currency", "calendar", "dateTimeField"],
                         undefined);
    }

    // Step 13.
    if (type === undefined) {
        ThrowTypeError(JSMSG_UNDEFINED_TYPE);
    }

    // Step 14.
    lazyDisplayNamesData.type = type;

    // Step 15.
    var fallback = GetOption(options, "fallback", "string", ["code", "none"], "code");

    // Step 16.
    lazyDisplayNamesData.fallback = fallback;

    // Step 24.
    var languageDisplay = GetOption(options, "languageDisplay", "string", ["dialect", "standard"],
                                    "dialect");

    // Step 25.
    if (type === "language") {
        lazyDisplayNamesData.languageDisplay = languageDisplay;
    }

    // We've done everything that must be done now: mark the lazy data as fully
    // computed and install it.
    initializeIntlObject(displayNames, "DisplayNames", lazyDisplayNamesData);
}

/**
 * Returns the subset of the given locale list for which this locale list has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 */
function Intl_DisplayNames_supportedLocalesOf(locales /*, options*/) {
    var options = arguments.length > 1 ? arguments[1] : undefined;

    // Step 1.
    var availableLocales = "DisplayNames";

    // Step 2.
    var requestedLocales = CanonicalizeLocaleList(locales);

    // Step 3.
    return SupportedLocales(availableLocales, requestedLocales, options);
}

/**
 * Returns the resolved options for a DisplayNames object.
 */
function Intl_DisplayNames_of(code) {
  // Step 1.
  var displayNames = this;

  // Steps 2-3.
  if (!IsObject(displayNames) || (displayNames = intl_GuardToDisplayNames(displayNames)) === null) {
      return callFunction(intl_CallDisplayNamesMethodIfWrapped, this, "Intl_DisplayNames_of");
  }

  code = ToString(code);

  var internals = getDisplayNamesInternals(displayNames);

  // Unpack the internals object to avoid a slow runtime to selfhosted JS call
  // in |intl_ComputeDisplayName()|.
  var {locale, calendar = "", style, type, languageDisplay = "", fallback} = internals;

  // Steps 5-10.
  return intl_ComputeDisplayName(displayNames, locale, calendar, style, languageDisplay, fallback,
                                 type, code);
}

/**
 * Returns the resolved options for a DisplayNames object.
 */
function Intl_DisplayNames_resolvedOptions() {
    // Step 1.
    var displayNames = this;

    // Steps 2-3.
    if (!IsObject(displayNames) || (displayNames = intl_GuardToDisplayNames(displayNames)) === null) {
        return callFunction(intl_CallDisplayNamesMethodIfWrapped, this,
                            "Intl_DisplayNames_resolvedOptions");
    }

    var internals = getDisplayNamesInternals(displayNames);

    // Steps 4-5.
    var options = {
        locale: internals.locale,
        style: internals.style,
        type: internals.type,
        fallback: internals.fallback,
    };

    // languageDisplay is only present for language display names.
    assert(hasOwn("languageDisplay", internals) === (internals.type === "language"),
           "languageDisplay is present iff type is 'language'");

    if (hasOwn("languageDisplay", internals)) {
        DefineDataProperty(options, "languageDisplay", internals.languageDisplay);
    }

    if (hasOwn("calendar", internals)) {
        DefineDataProperty(options, "calendar", internals.calendar);
    }

    // Step 6.
    return options;
}
