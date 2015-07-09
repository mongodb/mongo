/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Portions Copyright Norbert Lindenberg 2011-2012. */

/*global JSMSG_INTL_OBJECT_NOT_INITED: false, JSMSG_INVALID_LOCALES_ELEMENT: false,
         JSMSG_INVALID_LANGUAGE_TAG: false, JSMSG_INVALID_LOCALE_MATCHER: false,
         JSMSG_INVALID_OPTION_VALUE: false, JSMSG_INVALID_DIGITS_VALUE: false,
         JSMSG_INTL_OBJECT_REINITED: false, JSMSG_INVALID_CURRENCY_CODE: false,
         JSMSG_UNDEFINED_CURRENCY: false, JSMSG_INVALID_TIME_ZONE: false,
         JSMSG_DATE_NOT_FINITE: false,
         intl_Collator_availableLocales: false,
         intl_availableCollations: false,
         intl_CompareStrings: false,
         intl_NumberFormat_availableLocales: false,
         intl_numberingSystem: false,
         intl_FormatNumber: false,
         intl_DateTimeFormat_availableLocales: false,
         intl_availableCalendars: false,
         intl_patternForSkeleton: false,
         intl_FormatDateTime: false,
*/

/*
 * The Intl module specified by standard ECMA-402,
 * ECMAScript Internationalization API Specification.
 */


/********** Locales, Time Zones, and Currencies **********/


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
        var c = s[i];
        if ("a" <= c && c <= "z")
            c = callFunction(std_String_toUpperCase, c);
        result += c;
    }
    return result;
}

/**
 * Holder object for encapsulating regexp instances.
 *
 * Regular expression instances should be created after the initialization of
 * self-hosted global.
 */
var internalIntlRegExps = std_Object_create(null);
internalIntlRegExps.unicodeLocaleExtensionSequenceRE = null;
internalIntlRegExps.languageTagRE = null;
internalIntlRegExps.duplicateVariantRE = null;
internalIntlRegExps.duplicateSingletonRE = null;
internalIntlRegExps.isWellFormedCurrencyCodeRE = null;
internalIntlRegExps.currencyDigitsRE = null;

/**
 * Regular expression matching a "Unicode locale extension sequence", which the
 * specification defines as: "any substring of a language tag that starts with
 * a separator '-' and the singleton 'u' and includes the maximum sequence of
 * following non-singleton subtags and their preceding '-' separators."
 *
 * Alternatively, this may be defined as: the components of a language tag that
 * match the extension production in RFC 5646, where the singleton component is
 * "u".
 *
 * Spec: ECMAScript Internationalization API Specification, 6.2.1.
 */
function getUnicodeLocaleExtensionSequenceRE() {
    return internalIntlRegExps.unicodeLocaleExtensionSequenceRE ||
           (internalIntlRegExps.unicodeLocaleExtensionSequenceRE =
            regexp_construct_no_statics("-u(-[a-z0-9]{2,8})+"));
}


/**
 * Removes Unicode locale extension sequences from the given language tag.
 */
function removeUnicodeExtensions(locale) {
    // Don't use std_String_replace directly with a regular expression,
    // as that would set RegExp statics.
    var extensions;
    var unicodeLocaleExtensionSequenceRE = getUnicodeLocaleExtensionSequenceRE();
    while ((extensions = regexp_exec_no_statics(unicodeLocaleExtensionSequenceRE, locale)) !== null) {
        locale = callFunction(std_String_replace, locale, extensions[0], "");
        unicodeLocaleExtensionSequenceRE.lastIndex = 0;
    }
    return locale;
}


/**
 * Regular expression defining BCP 47 language tags.
 *
 * Spec: RFC 5646 section 2.1.
 */
function getLanguageTagRE() {
    if (internalIntlRegExps.languageTagRE)
        return internalIntlRegExps.languageTagRE;

    // RFC 5234 section B.1
    // ALPHA          =  %x41-5A / %x61-7A   ; A-Z / a-z
    var ALPHA = "[a-zA-Z]";
    // DIGIT          =  %x30-39
    //                        ; 0-9
    var DIGIT = "[0-9]";

    // RFC 5646 section 2.1
    // alphanum      = (ALPHA / DIGIT)     ; letters and numbers
    var alphanum = "(?:" + ALPHA + "|" + DIGIT + ")";
    // regular       = "art-lojban"        ; these tags match the 'langtag'
    //               / "cel-gaulish"       ; production, but their subtags
    //               / "no-bok"            ; are not extended language
    //               / "no-nyn"            ; or variant subtags: their meaning
    //               / "zh-guoyu"          ; is defined by their registration
    //               / "zh-hakka"          ; and all of these are deprecated
    //               / "zh-min"            ; in favor of a more modern
    //               / "zh-min-nan"        ; subtag or sequence of subtags
    //               / "zh-xiang"
    var regular = "(?:art-lojban|cel-gaulish|no-bok|no-nyn|zh-guoyu|zh-hakka|zh-min|zh-min-nan|zh-xiang)";
    // irregular     = "en-GB-oed"         ; irregular tags do not match
    //                / "i-ami"             ; the 'langtag' production and
    //                / "i-bnn"             ; would not otherwise be
    //                / "i-default"         ; considered 'well-formed'
    //                / "i-enochian"        ; These tags are all valid,
    //                / "i-hak"             ; but most are deprecated
    //                / "i-klingon"         ; in favor of more modern
    //                / "i-lux"             ; subtags or subtag
    //                / "i-mingo"           ; combination
    //                / "i-navajo"
    //                / "i-pwn"
    //                / "i-tao"
    //                / "i-tay"
    //                / "i-tsu"
    //                / "sgn-BE-FR"
    //                / "sgn-BE-NL"
    //                / "sgn-CH-DE"
    var irregular = "(?:en-GB-oed|i-ami|i-bnn|i-default|i-enochian|i-hak|i-klingon|i-lux|i-mingo|i-navajo|i-pwn|i-tao|i-tay|i-tsu|sgn-BE-FR|sgn-BE-NL|sgn-CH-DE)";
    // grandfathered = irregular           ; non-redundant tags registered
    //               / regular             ; during the RFC 3066 era
    var grandfathered = "(?:" + irregular + "|" + regular + ")";
    // privateuse    = "x" 1*("-" (1*8alphanum))
    var privateuse = "(?:x(?:-[a-z0-9]{1,8})+)";
    // singleton     = DIGIT               ; 0 - 9
    //               / %x41-57             ; A - W
    //               / %x59-5A             ; Y - Z
    //               / %x61-77             ; a - w
    //               / %x79-7A             ; y - z
    var singleton = "(?:" + DIGIT + "|[A-WY-Za-wy-z])";
    // extension     = singleton 1*("-" (2*8alphanum))
    var extension = "(?:" + singleton + "(?:-" + alphanum + "{2,8})+)";
    // variant       = 5*8alphanum         ; registered variants
    //               / (DIGIT 3alphanum)
    var variant = "(?:" + alphanum + "{5,8}|(?:" + DIGIT + alphanum + "{3}))";
    // region        = 2ALPHA              ; ISO 3166-1 code
    //               / 3DIGIT              ; UN M.49 code
    var region = "(?:" + ALPHA + "{2}|" + DIGIT + "{3})";
    // script        = 4ALPHA              ; ISO 15924 code
    var script = "(?:" + ALPHA + "{4})";
    // extlang       = 3ALPHA              ; selected ISO 639 codes
    //                 *2("-" 3ALPHA)      ; permanently reserved
    var extlang = "(?:" + ALPHA + "{3}(?:-" + ALPHA + "{3}){0,2})";
    // language      = 2*3ALPHA            ; shortest ISO 639 code
    //                 ["-" extlang]       ; sometimes followed by
    //                                     ; extended language subtags
    //               / 4ALPHA              ; or reserved for future use
    //               / 5*8ALPHA            ; or registered language subtag
    var language = "(?:" + ALPHA + "{2,3}(?:-" + extlang + ")?|" + ALPHA + "{4}|" + ALPHA + "{5,8})";
    // langtag       = language
    //                 ["-" script]
    //                 ["-" region]
    //                 *("-" variant)
    //                 *("-" extension)
    //                 ["-" privateuse]
    var langtag = language + "(?:-" + script + ")?(?:-" + region + ")?(?:-" +
                  variant + ")*(?:-" + extension + ")*(?:-" + privateuse + ")?";
    // Language-Tag  = langtag             ; normal language tags
    //               / privateuse          ; private use tag
    //               / grandfathered       ; grandfathered tags
    var languageTag = "^(?:" + langtag + "|" + privateuse + "|" + grandfathered + ")$";

    // Language tags are case insensitive (RFC 5646 section 2.1.1).
    return (internalIntlRegExps.languageTagRE =
            regexp_construct_no_statics(languageTag, "i"));
}


function getDuplicateVariantRE() {
    if (internalIntlRegExps.duplicateVariantRE)
        return internalIntlRegExps.duplicateVariantRE;

    // RFC 5234 section B.1
    // ALPHA          =  %x41-5A / %x61-7A   ; A-Z / a-z
    var ALPHA = "[a-zA-Z]";
    // DIGIT          =  %x30-39
    //                        ; 0-9
    var DIGIT = "[0-9]";

    // RFC 5646 section 2.1
    // alphanum      = (ALPHA / DIGIT)     ; letters and numbers
    var alphanum = "(?:" + ALPHA + "|" + DIGIT + ")";
    // variant       = 5*8alphanum         ; registered variants
    //               / (DIGIT 3alphanum)
    var variant = "(?:" + alphanum + "{5,8}|(?:" + DIGIT + alphanum + "{3}))";

    // Match a langtag that contains a duplicate variant.
    var duplicateVariant =
        // Match everything in a langtag prior to any variants, and maybe some
        // of the variants as well (which makes this pattern inefficient but
        // not wrong, for our purposes);
        "(?:" + alphanum + "{2,8}-)+" +
        // a variant, parenthesised so that we can refer back to it later;
        "(" + variant + ")-" +
        // zero or more subtags at least two characters long (thus stopping
        // before extension and privateuse components);
        "(?:" + alphanum + "{2,8}-)*" +
        // and the same variant again
        "\\1" +
        // ...but not followed by any characters that would turn it into a
        // different subtag.
        "(?!" + alphanum + ")";

    // Language tags are case insensitive (RFC 5646 section 2.1.1), but for
    // this regular expression that's covered by having its character classes
    // list both upper- and lower-case characters.
    return (internalIntlRegExps.duplicateVariantRE =
            regexp_construct_no_statics(duplicateVariant));
}


function getDuplicateSingletonRE() {
    if (internalIntlRegExps.duplicateSingletonRE)
        return internalIntlRegExps.duplicateSingletonRE;

    // RFC 5234 section B.1
    // ALPHA          =  %x41-5A / %x61-7A   ; A-Z / a-z
    var ALPHA = "[a-zA-Z]";
    // DIGIT          =  %x30-39
    //                        ; 0-9
    var DIGIT = "[0-9]";

    // RFC 5646 section 2.1
    // alphanum      = (ALPHA / DIGIT)     ; letters and numbers
    var alphanum = "(?:" + ALPHA + "|" + DIGIT + ")";
    // singleton     = DIGIT               ; 0 - 9
    //               / %x41-57             ; A - W
    //               / %x59-5A             ; Y - Z
    //               / %x61-77             ; a - w
    //               / %x79-7A             ; y - z
    var singleton = "(?:" + DIGIT + "|[A-WY-Za-wy-z])";

    // Match a langtag that contains a duplicate singleton.
    var duplicateSingleton =
        // Match a singleton subtag, parenthesised so that we can refer back to
        // it later;
        "-(" + singleton + ")-" +
        // then zero or more subtags;
        "(?:" + alphanum + "+-)*" +
        // and the same singleton again
        "\\1" +
        // ...but not followed by any characters that would turn it into a
        // different subtag.
        "(?!" + alphanum + ")";

    // Language tags are case insensitive (RFC 5646 section 2.1.1), but for
    // this regular expression that's covered by having its character classes
    // list both upper- and lower-case characters.
    return (internalIntlRegExps.duplicateSingletonRE =
            regexp_construct_no_statics(duplicateSingleton));
}


/**
 * Verifies that the given string is a well-formed BCP 47 language tag
 * with no duplicate variant or singleton subtags.
 *
 * Spec: ECMAScript Internationalization API Specification, 6.2.2.
 */
function IsStructurallyValidLanguageTag(locale) {
    assert(typeof locale === "string", "IsStructurallyValidLanguageTag");
    var languageTagRE = getLanguageTagRE();
    if (!regexp_test_no_statics(languageTagRE, locale))
        return false;

    // Before checking for duplicate variant or singleton subtags with
    // regular expressions, we have to get private use subtag sequences
    // out of the picture.
    if (callFunction(std_String_startsWith, locale, "x-"))
        return true;
    var pos = callFunction(std_String_indexOf, locale, "-x-");
    if (pos !== -1)
        locale = callFunction(std_String_substring, locale, 0, pos);

    // Check for duplicate variant or singleton subtags.
    var duplicateVariantRE = getDuplicateVariantRE();
    var duplicateSingletonRE = getDuplicateSingletonRE();
    return !regexp_test_no_statics(duplicateVariantRE, locale) &&
           !regexp_test_no_statics(duplicateSingletonRE, locale);
}


/**
 * Canonicalizes the given structurally valid BCP 47 language tag, including
 * regularized case of subtags. For example, the language tag
 * Zh-NAN-haNS-bu-variant2-Variant1-u-ca-chinese-t-Zh-laTN-x-PRIVATE, where
 *
 *     Zh             ; 2*3ALPHA
 *     -NAN           ; ["-" extlang]
 *     -haNS          ; ["-" script]
 *     -bu            ; ["-" region]
 *     -variant2      ; *("-" variant)
 *     -Variant1
 *     -u-ca-chinese  ; *("-" extension)
 *     -t-Zh-laTN
 *     -x-PRIVATE     ; ["-" privateuse]
 *
 * becomes nan-Hans-mm-variant2-variant1-t-zh-latn-u-ca-chinese-x-private
 *
 * Spec: ECMAScript Internationalization API Specification, 6.2.3.
 * Spec: RFC 5646, section 4.5.
 */
function CanonicalizeLanguageTag(locale) {
    assert(IsStructurallyValidLanguageTag(locale), "CanonicalizeLanguageTag");

    // The input
    // "Zh-NAN-haNS-bu-variant2-Variant1-u-ca-chinese-t-Zh-laTN-x-PRIVATE"
    // will be used throughout this method to illustrate how it works.

    // Language tags are compared and processed case-insensitively, so
    // technically it's not necessary to adjust case. But for easier processing,
    // and because the canonical form for most subtags is lower case, we start
    // with lower case for all.
    // "Zh-NAN-haNS-bu-variant2-Variant1-u-ca-chinese-t-Zh-laTN-x-PRIVATE" ->
    // "zh-nan-hans-bu-variant2-variant1-u-ca-chinese-t-zh-latn-x-private"
    locale = callFunction(std_String_toLowerCase, locale);

    // Handle mappings for complete tags.
    if (callFunction(std_Object_hasOwnProperty, langTagMappings, locale))
        return langTagMappings[locale];

    var subtags = callFunction(std_String_split, locale, "-");
    var i = 0;

    // Handle the standard part: All subtags before the first singleton or "x".
    // "zh-nan-hans-bu-variant2-variant1"
    while (i < subtags.length) {
        var subtag = subtags[i];

        // If we reach the start of an extension sequence or private use part,
        // we're done with this loop. We have to check for i > 0 because for
        // irregular language tags, such as i-klingon, the single-character
        // subtag "i" is not the start of an extension sequence.
        // In the example, we break at "u".
        if (subtag.length === 1 && (i > 0 || subtag === "x"))
            break;

        if (subtag.length === 4) {
            // 4-character subtags are script codes; their first character
            // needs to be capitalized. "hans" -> "Hans"
            subtag = callFunction(std_String_toUpperCase, subtag[0]) +
                     callFunction(std_String_substring, subtag, 1);
        } else if (i !== 0 && subtag.length === 2) {
            // 2-character subtags that are not in initial position are region
            // codes; they need to be upper case. "bu" -> "BU"
            subtag = callFunction(std_String_toUpperCase, subtag);
        }
        if (callFunction(std_Object_hasOwnProperty, langSubtagMappings, subtag)) {
            // Replace deprecated subtags with their preferred values.
            // "BU" -> "MM"
            // This has to come after we capitalize region codes because
            // otherwise some language and region codes could be confused.
            // For example, "in" is an obsolete language code for Indonesian,
            // but "IN" is the country code for India.
            // Note that the script generating langSubtagMappings makes sure
            // that no regular subtag mapping will replace an extlang code.
            subtag = langSubtagMappings[subtag];
        } else if (callFunction(std_Object_hasOwnProperty, extlangMappings, subtag)) {
            // Replace deprecated extlang subtags with their preferred values,
            // and remove the preceding subtag if it's a redundant prefix.
            // "zh-nan" -> "nan"
            // Note that the script generating extlangMappings makes sure that
            // no extlang mapping will replace a normal language code.
            subtag = extlangMappings[subtag].preferred;
            if (i === 1 && extlangMappings[subtag].prefix === subtags[0]) {
                callFunction(std_Array_shift, subtags);
                i--;
            }
        }
        subtags[i] = subtag;
        i++;
    }
    var normal = callFunction(std_Array_join, callFunction(std_Array_slice, subtags, 0, i), "-");

    // Extension sequences are sorted by their singleton characters.
    // "u-ca-chinese-t-zh-latn" -> "t-zh-latn-u-ca-chinese"
    var extensions = new List();
    while (i < subtags.length && subtags[i] !== "x") {
        var extensionStart = i;
        i++;
        while (i < subtags.length && subtags[i].length > 1)
            i++;
        var extension = callFunction(std_Array_join, callFunction(std_Array_slice, subtags, extensionStart, i), "-");
        extensions.push(extension);
    }
    extensions.sort();

    // Private use sequences are left as is. "x-private"
    var privateUse = "";
    if (i < subtags.length)
        privateUse = callFunction(std_Array_join, callFunction(std_Array_slice, subtags, i), "-");

    // Put everything back together.
    var canonical = normal;
    if (extensions.length > 0)
        canonical += "-" + extensions.join("-");
    if (privateUse.length > 0) {
        // Be careful of a Language-Tag that is entirely privateuse.
        if (canonical.length > 0)
            canonical += "-" + privateUse;
        else
            canonical = privateUse;
    }

    return canonical;
}


// mappings from some commonly used old-style language tags to current flavors
// with script codes
var oldStyleLanguageTagMappings = {
    "pa-PK": "pa-Arab-PK",
    "zh-CN": "zh-Hans-CN",
    "zh-HK": "zh-Hant-HK",
    "zh-SG": "zh-Hans-SG",
    "zh-TW": "zh-Hant-TW"
};


/**
 * Returns the BCP 47 language tag for the host environment's current locale.
 *
 * Spec: ECMAScript Internationalization API Specification, 6.2.4.
 */
function DefaultLocale() {
    // The locale of last resort is used if none of the available locales
    // satisfies a request. "en-GB" is used based on the assumptions that
    // English is the most common second language, that both en-GB and en-US
    // are normally available in an implementation, and that en-GB is more
    // representative of the English used in other locales.
    var localeOfLastResort = "en-GB";

    var locale = RuntimeDefaultLocale();
    if (!IsStructurallyValidLanguageTag(locale))
        return localeOfLastResort;

    locale = CanonicalizeLanguageTag(locale);
    if (callFunction(std_Object_hasOwnProperty, oldStyleLanguageTagMappings, locale))
        locale = oldStyleLanguageTagMappings[locale];

    if (!(collatorInternalProperties.availableLocales()[locale] &&
          numberFormatInternalProperties.availableLocales()[locale] &&
          dateTimeFormatInternalProperties.availableLocales()[locale]))
    {
        locale = localeOfLastResort;
    }
    return locale;
}


/**
 * Verifies that the given string is a well-formed ISO 4217 currency code.
 *
 * Spec: ECMAScript Internationalization API Specification, 6.3.1.
 */
function getIsWellFormedCurrencyCodeRE() {
    return internalIntlRegExps.isWellFormedCurrencyCodeRE ||
           (internalIntlRegExps.isWellFormedCurrencyCodeRE =
            regexp_construct_no_statics("[^A-Z]"));
}
function IsWellFormedCurrencyCode(currency) {
    var c = ToString(currency);
    var normalized = toASCIIUpperCase(c);
    if (normalized.length !== 3)
        return false;
    return !regexp_test_no_statics(getIsWellFormedCurrencyCodeRE(), normalized);
}


/********** Locale and Parameter Negotiation **********/


/**
 * Add old-style language tags without script code for locales that in current
 * usage would include a script subtag. Returns the availableLocales argument
 * provided.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.1.
 */
function addOldStyleLanguageTags(availableLocales) {
    var oldStyleLocales = std_Object_getOwnPropertyNames(oldStyleLanguageTagMappings);
    for (var i = 0; i < oldStyleLocales.length; i++) {
        var oldStyleLocale = oldStyleLocales[i];
        if (availableLocales[oldStyleLanguageTagMappings[oldStyleLocale]])
            availableLocales[oldStyleLocale] = true;
    }
    return availableLocales;
}


/**
 * Canonicalizes a locale list.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.1.
 */
function CanonicalizeLocaleList(locales) {
    if (locales === undefined)
        return new List();
    var seen = new List();
    if (typeof locales === "string")
        locales = [locales];
    var O = ToObject(locales);
    var len = TO_UINT32(O.length);
    var k = 0;
    while (k < len) {
        // Don't call ToString(k) - SpiderMonkey is faster with integers.
        var kPresent = HasProperty(O, k);
        if (kPresent) {
            var kValue = O[k];
            if (!(typeof kValue === "string" || IsObject(kValue)))
                ThrowError(JSMSG_INVALID_LOCALES_ELEMENT);
            var tag = ToString(kValue);
            if (!IsStructurallyValidLanguageTag(tag))
                ThrowError(JSMSG_INVALID_LANGUAGE_TAG, tag);
            tag = CanonicalizeLanguageTag(tag);
            if (seen.indexOf(tag) === -1)
                seen.push(tag);
        }
        k++;
    }
    return seen;
}


/**
 * Compares a BCP 47 language tag against the locales in availableLocales
 * and returns the best available match. Uses the fallback
 * mechanism of RFC 4647, section 3.4.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.2.
 * Spec: RFC 4647, section 3.4.
 */
function BestAvailableLocale(availableLocales, locale) {
    assert(IsStructurallyValidLanguageTag(locale), "invalid BestAvailableLocale locale structure");
    assert(locale === CanonicalizeLanguageTag(locale), "non-canonical BestAvailableLocale locale");
    assert(callFunction(std_String_indexOf, locale, "-u-") === -1, "locale shouldn't contain -u-");

    var candidate = locale;
    while (true) {
        if (availableLocales[candidate])
            return candidate;
        var pos = callFunction(std_String_lastIndexOf, candidate, "-");
        if (pos === -1)
            return undefined;
        if (pos >= 2 && candidate[pos - 2] === "-")
            pos -= 2;
        candidate = callFunction(std_String_substring, candidate, 0, pos);
    }
}


/**
 * Compares a BCP 47 language priority list against the set of locales in
 * availableLocales and determines the best available language to meet the
 * request. Options specified through Unicode extension subsequences are
 * ignored in the lookup, but information about such subsequences is returned
 * separately.
 *
 * This variant is based on the Lookup algorithm of RFC 4647 section 3.4.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.3.
 * Spec: RFC 4647, section 3.4.
 */
function LookupMatcher(availableLocales, requestedLocales) {
    var i = 0;
    var len = requestedLocales.length;
    var availableLocale;
    var locale, noExtensionsLocale;
    while (i < len && availableLocale === undefined) {
        locale = requestedLocales[i];
        noExtensionsLocale = removeUnicodeExtensions(locale);
        availableLocale = BestAvailableLocale(availableLocales, noExtensionsLocale);
        i++;
    }

    var result = new Record();
    if (availableLocale !== undefined) {
        result.locale = availableLocale;
        if (locale !== noExtensionsLocale) {
            var unicodeLocaleExtensionSequenceRE = getUnicodeLocaleExtensionSequenceRE();
            var extensionMatch = regexp_exec_no_statics(unicodeLocaleExtensionSequenceRE, locale);
            var extension = extensionMatch[0];
            var extensionIndex = extensionMatch.index;
            result.extension = extension;
            result.extensionIndex = extensionIndex;
        }
    } else {
        result.locale = DefaultLocale();
    }
    return result;
}


/**
 * Compares a BCP 47 language priority list against the set of locales in
 * availableLocales and determines the best available language to meet the
 * request. Options specified through Unicode extension subsequences are
 * ignored in the lookup, but information about such subsequences is returned
 * separately.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.4.
 */
function BestFitMatcher(availableLocales, requestedLocales) {
    // this implementation doesn't have anything better
    return LookupMatcher(availableLocales, requestedLocales);
}


/**
 * Compares a BCP 47 language priority list against availableLocales and
 * determines the best available language to meet the request. Options specified
 * through Unicode extension subsequences are negotiated separately, taking the
 * caller's relevant extensions and locale data as well as client-provided
 * options into consideration.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.5.
 */
function ResolveLocale(availableLocales, requestedLocales, options, relevantExtensionKeys, localeData) {
    /*jshint laxbreak: true */

    // Steps 1-3.
    var matcher = options.localeMatcher;
    var r = (matcher === "lookup")
            ? LookupMatcher(availableLocales, requestedLocales)
            : BestFitMatcher(availableLocales, requestedLocales);

    // Step 4.
    var foundLocale = r.locale;

    // Step 5.a.
    var extension = r.extension;
    var extensionIndex, extensionSubtags, extensionSubtagsLength;

    // Step 5.
    if (extension !== undefined) {
        // Step 5.b.
        extensionIndex = r.extensionIndex;

        // Steps 5.d-e.
        extensionSubtags = callFunction(std_String_split, extension, "-");
        extensionSubtagsLength = extensionSubtags.length;
    }

    // Steps 6-7.
    var result = new Record();
    result.dataLocale = foundLocale;

    // Step 8.
    var supportedExtension = "-u";

    // Steps 9-11.
    var i = 0;
    var len = relevantExtensionKeys.length;
    while (i < len) {
        // Steps 11.a-c.
        var key = relevantExtensionKeys[i];

        // In this implementation, localeData is a function, not an object.
        var foundLocaleData = localeData(foundLocale);
        var keyLocaleData = foundLocaleData[key];

        // Locale data provides default value.
        // Step 11.d.
        var value = keyLocaleData[0];

        // Locale tag may override.

        // Step 11.e.
        var supportedExtensionAddition = "";

        // Step 11.f is implemented by Utilities.js.

        var valuePos;

        // Step 11.g.
        if (extensionSubtags !== undefined) {
            // Step 11.g.i.
            var keyPos = callFunction(std_Array_indexOf, extensionSubtags, key);

            // Step 11.g.ii.
            if (keyPos !== -1) {
                // Step 11.g.ii.1.
                if (keyPos + 1 < extensionSubtagsLength &&
                    extensionSubtags[keyPos + 1].length > 2)
                {
                    // Step 11.g.ii.1.a.
                    var requestedValue = extensionSubtags[keyPos + 1];

                    // Step 11.g.ii.1.b.
                    valuePos = callFunction(std_Array_indexOf, keyLocaleData, requestedValue);

                    // Step 11.g.ii.1.c.
                    if (valuePos !== -1) {
                        value = requestedValue;
                        supportedExtensionAddition = "-" + key + "-" + value;
                    }
                } else {
                    // Step 11.g.ii.2.

                    // According to the LDML spec, if there's no type value,
                    // and true is an allowed value, it's used.

                    // Step 11.g.ii.2.a.
                    valuePos = callFunction(std_Array_indexOf, keyLocaleData, "true");

                    // Step 11.g.ii.2.b.
                    if (valuePos !== -1)
                        value = "true";
                }
            }
        }

        // Options override all.

        // Step 11.h.i.
        var optionsValue = options[key];

        // Step 11.h, 11.h.ii.
        if (optionsValue !== undefined &&
            callFunction(std_Array_indexOf, keyLocaleData, optionsValue) !== -1)
        {
            // Step 11.h.ii.1.
            if (optionsValue !== value) {
                value = optionsValue;
                supportedExtensionAddition = "";
            }
        }

        // Steps 11.i-k.
        result[key] = value;
        supportedExtension += supportedExtensionAddition;
        i++;
    }

    // Step 12.
    if (supportedExtension.length > 2) {
        var preExtension = callFunction(std_String_substring, foundLocale, 0, extensionIndex);
        var postExtension = callFunction(std_String_substring, foundLocale, extensionIndex);
        foundLocale = preExtension + supportedExtension + postExtension;
    }

    // Steps 13-14.
    result.locale = foundLocale;
    return result;
}


/**
 * Returns the subset of requestedLocales for which availableLocales has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.6.
 */
function LookupSupportedLocales(availableLocales, requestedLocales) {
    // Steps 1-2.
    var len = requestedLocales.length;
    var subset = new List();

    // Steps 3-4.
    var k = 0;
    while (k < len) {
        // Steps 4.a-b.
        var locale = requestedLocales[k];
        var noExtensionsLocale = removeUnicodeExtensions(locale);

        // Step 4.c-d.
        var availableLocale = BestAvailableLocale(availableLocales, noExtensionsLocale);
        if (availableLocale !== undefined)
            subset.push(locale);

        // Step 4.e.
        k++;
    }

    // Steps 5-6.
    return subset.slice(0);
}


/**
 * Returns the subset of requestedLocales for which availableLocales has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.7.
 */
function BestFitSupportedLocales(availableLocales, requestedLocales) {
    // don't have anything better
    return LookupSupportedLocales(availableLocales, requestedLocales);
}


/**
 * Returns the subset of requestedLocales for which availableLocales has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.8.
 */
function SupportedLocales(availableLocales, requestedLocales, options) {
    /*jshint laxbreak: true */

    // Step 1.
    var matcher;
    if (options !== undefined) {
        // Steps 1.a-b.
        options = ToObject(options);
        matcher = options.localeMatcher;

        // Step 1.c.
        if (matcher !== undefined) {
            matcher = ToString(matcher);
            if (matcher !== "lookup" && matcher !== "best fit")
                ThrowError(JSMSG_INVALID_LOCALE_MATCHER, matcher);
        }
    }

    // Steps 2-3.
    var subset = (matcher === undefined || matcher === "best fit")
                 ? BestFitSupportedLocales(availableLocales, requestedLocales)
                 : LookupSupportedLocales(availableLocales, requestedLocales);

    // Step 4.
    for (var i = 0; i < subset.length; i++) {
        _DefineDataProperty(subset, i, subset[i],
                            ATTR_ENUMERABLE | ATTR_NONCONFIGURABLE | ATTR_NONWRITABLE);
    }
    _DefineDataProperty(subset, "length", subset.length,
                        ATTR_NONENUMERABLE | ATTR_NONCONFIGURABLE | ATTR_NONWRITABLE);

    // Step 5.
    return subset;
}


/**
 * Extracts a property value from the provided options object, converts it to
 * the required type, checks whether it is one of a list of allowed values,
 * and fills in a fallback value if necessary.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.9.
 */
function GetOption(options, property, type, values, fallback) {
    // Step 1.
    var value = options[property];

    // Step 2.
    if (value !== undefined) {
        // Steps 2.a-c.
        if (type === "boolean")
            value = ToBoolean(value);
        else if (type === "string")
            value = ToString(value);
        else
            assert(false, "GetOption");

        // Step 2.d.
        if (values !== undefined && callFunction(std_Array_indexOf, values, value) === -1)
            ThrowError(JSMSG_INVALID_OPTION_VALUE, property, value);

        // Step 2.e.
        return value;
    }

    // Step 3.
    return fallback;
}

/**
 * Extracts a property value from the provided options object, converts it to a
 * Number value, checks whether it is in the allowed range, and fills in a
 * fallback value if necessary.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.10.
 */
function GetNumberOption(options, property, minimum, maximum, fallback) {
    assert(typeof minimum === "number", "GetNumberOption");
    assert(typeof maximum === "number", "GetNumberOption");
    assert(fallback === undefined || (fallback >= minimum && fallback <= maximum), "GetNumberOption");

    // Step 1.
    var value = options[property];

    // Step 2.
    if (value !== undefined) {
        value = ToNumber(value);
        if (Number_isNaN(value) || value < minimum || value > maximum)
            ThrowError(JSMSG_INVALID_DIGITS_VALUE, value);
        return std_Math_floor(value);
    }

    // Step 3.
    return fallback;
}


/********** Property access for Intl objects **********/


/**
 * Set a normal public property p of o to value v, but use Object.defineProperty
 * to avoid interference from setters on Object.prototype.
 */
function defineProperty(o, p, v) {
    _DefineDataProperty(o, p, v, ATTR_ENUMERABLE | ATTR_CONFIGURABLE | ATTR_WRITABLE);
}


/**
 * Weak map used to track the initialize-as-Intl status (and, if an object has
 * been so initialized, the Intl-specific internal properties) of all objects.
 * Presence of an object as a key within this map indicates that the object has
 * its [[initializedIntlObject]] internal property set to true.  The associated
 * value is an object whose structure is documented in |initializeIntlObject|
 * below.
 *
 * Ideally we'd be using private symbols for internal properties, but
 * SpiderMonkey doesn't have those yet.
 */
var internalsMap = new WeakMap();


/**
 * Set the [[initializedIntlObject]] internal property of |obj| to true.
 */
function initializeIntlObject(obj) {
    assert(IsObject(obj), "Non-object passed to initializeIntlObject");

    // Intl-initialized objects are weird.  They have [[initializedIntlObject]]
    // set on them, but they don't *necessarily* have any other properties.

    var internals = std_Object_create(null);

    // The meaning of an internals object for an object |obj| is as follows.
    //
    // If the .type is "partial", |obj| has [[initializedIntlObject]] set but
    // nothing else.  No other property of |internals| can be used.  (This
    // occurs when InitializeCollator or similar marks an object as
    // [[initializedIntlObject]] but fails before marking it as the appropriate
    // more-specific type ["Collator", "DateTimeFormat", "NumberFormat"].)
    //
    // Otherwise, the .type indicates the type of Intl object that |obj| is:
    // "Collator", "DateTimeFormat", or "NumberFormat" (likely with more coming
    // in future Intl specs).  In these cases |obj| *conceptually* also has
    // [[initializedCollator]] or similar set, and all the other properties
    // implied by that.
    //
    // If |internals| doesn't have a "partial" .type, two additional properties
    // have meaning.  The .lazyData property stores information needed to
    // compute -- without observable side effects -- the actual internal Intl
    // properties of |obj|.  If it is non-null, then the actual internal
    // properties haven't been computed, and .lazyData must be processed by
    // |setInternalProperties| before internal Intl property values are
    // available.  If it is null, then the .internalProps property contains an
    // object whose properties are the internal Intl properties of |obj|.

    internals.type = "partial";
    internals.lazyData = null;
    internals.internalProps = null;

    callFunction(std_WeakMap_set, internalsMap, obj, internals);
    return internals;
}


/**
 * Mark |internals| as having the given type and lazy data.
 */
function setLazyData(internals, type, lazyData)
{
    assert(internals.type === "partial", "can't set lazy data for anything but a newborn");
    assert(type === "Collator" || type === "DateTimeFormat" || type == "NumberFormat", "bad type");
    assert(IsObject(lazyData), "non-object lazy data");

    // Set in reverse order so that the .type change is a barrier.
    internals.lazyData = lazyData;
    internals.type = type;
}


/**
 * Set the internal properties object for an |internals| object previously
 * associated with lazy data.
 */
function setInternalProperties(internals, internalProps)
{
    assert(internals.type !== "partial", "newborn internals can't have computed internals");
    assert(IsObject(internals.lazyData), "lazy data must exist already");
    assert(IsObject(internalProps), "internalProps argument should be an object");

    // Set in reverse order so that the .lazyData nulling is a barrier.
    internals.internalProps = internalProps;
    internals.lazyData = null;
}


/**
 * Get the existing internal properties out of a non-newborn |internals|, or
 * null if none have been computed.
 */
function maybeInternalProperties(internals)
{
    assert(IsObject(internals), "non-object passed to maybeInternalProperties");
    assert(internals.type !== "partial", "maybeInternalProperties must only be used on completely-initialized internals objects");
    var lazyData = internals.lazyData;
    if (lazyData)
        return null;
    assert(IsObject(internals.internalProps), "missing lazy data and computed internals");
    return internals.internalProps;
}


/**
 * Return whether |obj| has an[[initializedIntlObject]] property set to true.
 */
function isInitializedIntlObject(obj) {
#ifdef DEBUG
    var internals = callFunction(std_WeakMap_get, internalsMap, obj);
    if (IsObject(internals)) {
        assert(callFunction(std_Object_hasOwnProperty, internals, "type"), "missing type");
        var type = internals.type;
        assert(type === "partial" || type === "Collator" || type === "DateTimeFormat" || type === "NumberFormat", "unexpected type");
        assert(callFunction(std_Object_hasOwnProperty, internals, "lazyData"), "missing lazyData");
        assert(callFunction(std_Object_hasOwnProperty, internals, "internalProps"), "missing internalProps");
    } else {
        assert(internals === undefined, "bad mapping for |obj|");
    }
#endif
    return callFunction(std_WeakMap_has, internalsMap, obj);
}


/**
 * Check that |obj| meets the requirements for "this Collator object", "this
 * NumberFormat object", or "this DateTimeFormat object" as used in the method
 * with the given name.  Throw a TypeError if |obj| doesn't meet these
 * requirements.  But if it does, return |obj|'s internals object (*not* the
 * object holding its internal properties!), associated with it by
 * |internalsMap|, with structure specified above.
 *
 * Spec: ECMAScript Internationalization API Specification, 10.3.
 * Spec: ECMAScript Internationalization API Specification, 11.3.
 * Spec: ECMAScript Internationalization API Specification, 12.3.
 */
function getIntlObjectInternals(obj, className, methodName) {
    assert(typeof className === "string", "bad className for getIntlObjectInternals");

    var internals = callFunction(std_WeakMap_get, internalsMap, obj);
    assert(internals === undefined || isInitializedIntlObject(obj), "bad mapping in internalsMap");

    if (internals === undefined || internals.type !== className)
        ThrowError(JSMSG_INTL_OBJECT_NOT_INITED, className, methodName, className);

    return internals;
}


/**
 * Get the internal properties of known-Intl object |obj|.  For use only by
 * C++ code that knows what it's doing!
 */
function getInternals(obj)
{
    assert(isInitializedIntlObject(obj), "for use only on guaranteed Intl objects");

    var internals = callFunction(std_WeakMap_get, internalsMap, obj);

    assert(internals.type !== "partial", "must have been successfully initialized");
    var lazyData = internals.lazyData;
    if (!lazyData)
        return internals.internalProps;

    var internalProps;
    var type = internals.type;
    if (type === "Collator")
        internalProps = resolveCollatorInternals(lazyData)
    else if (type === "DateTimeFormat")
        internalProps = resolveDateTimeFormatInternals(lazyData)
    else
        internalProps = resolveNumberFormatInternals(lazyData);
    setInternalProperties(internals, internalProps);
    return internalProps;
}


/********** Intl.Collator **********/


/**
 * Mapping from Unicode extension keys for collation to options properties,
 * their types and permissible values.
 *
 * Spec: ECMAScript Internationalization API Specification, 10.1.1.
 */
var collatorKeyMappings = {
    kn: {property: "numeric", type: "boolean"},
    kf: {property: "caseFirst", type: "string", values: ["upper", "lower", "false"]}
};


/**
 * Compute an internal properties object from |lazyCollatorData|.
 */
function resolveCollatorInternals(lazyCollatorData)
{
    assert(IsObject(lazyCollatorData), "lazy data not an object?");

    var internalProps = std_Object_create(null);

    // Step 7.
    internalProps.usage = lazyCollatorData.usage;

    // Step 8.
    var Collator = collatorInternalProperties;

    // Step 9.
    var collatorIsSorting = lazyCollatorData.usage === "sort";
    var localeData = collatorIsSorting
                     ? Collator.sortLocaleData
                     : Collator.searchLocaleData;

    // Compute effective locale.
    // Step 14.
    var relevantExtensionKeys = Collator.relevantExtensionKeys;

    // Step 15.
    var r = ResolveLocale(Collator.availableLocales(),
                          lazyCollatorData.requestedLocales,
                          lazyCollatorData.opt,
                          relevantExtensionKeys,
                          localeData);

    // Step 16.
    internalProps.locale = r.locale;

    // Steps 17-19.
    var key, property, value, mapping;
    var i = 0, len = relevantExtensionKeys.length;
    while (i < len) {
        // Step 19.a.
        key = relevantExtensionKeys[i];
        if (key === "co") {
            // Step 19.b.
            property = "collation";
            value = r.co === null ? "default" : r.co;
        } else {
            // Step 19.c.
            mapping = collatorKeyMappings[key];
            property = mapping.property;
            value = r[key];
            if (mapping.type === "boolean")
                value = value === "true";
        }

        // Step 19.d.
        internalProps[property] = value;

        // Step 19.e.
        i++;
    }

    // Compute remaining collation options.
    // Steps 21-22.
    var s = lazyCollatorData.rawSensitivity;
    if (s === undefined) {
        if (collatorIsSorting) {
            // Step 21.a.
            s = "variant";
        } else {
            // Step 21.b.
            var dataLocale = r.dataLocale;
            var dataLocaleData = localeData(dataLocale);
            s = dataLocaleData.sensitivity;
        }
    }
    internalProps.sensitivity = s;

    // Step 24.
    internalProps.ignorePunctuation = lazyCollatorData.ignorePunctuation;

    // Step 25.
    internalProps.boundFormat = undefined;

    // The caller is responsible for associating |internalProps| with the right
    // object using |setInternalProperties|.
    return internalProps;
}


/**
 * Returns an object containing the Collator internal properties of |obj|, or
 * throws a TypeError if |obj| isn't Collator-initialized.
 */
function getCollatorInternals(obj, methodName) {
    var internals = getIntlObjectInternals(obj, "Collator", methodName);
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
    assert(IsObject(collator), "InitializeCollator");

    // Step 1.
    if (isInitializedIntlObject(collator))
        ThrowError(JSMSG_INTL_OBJECT_REINITED);

    // Step 2.
    var internals = initializeIntlObject(collator);

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

    // Step 3.
    var requestedLocales = CanonicalizeLocaleList(locales);
    lazyCollatorData.requestedLocales = requestedLocales;

    // Steps 4-5.
    //
    // If we ever need more speed here at startup, we should try to detect the
    // case where |options === undefined| and Object.prototype hasn't been
    // mucked with.  (|options| is fully consumed in this method, so it's not a
    // concern that Object.prototype might be touched between now and when
    // |resolveCollatorInternals| is called.)  For now, just keep it simple.
    if (options === undefined)
        options = {};
    else
        options = ToObject(options);

    // Compute options that impact interpretation of locale.
    // Step 6.
    var u = GetOption(options, "usage", "string", ["sort", "search"], "sort");
    lazyCollatorData.usage = u;

    // Step 10.
    var opt = new Record();
    lazyCollatorData.opt = opt;

    // Steps 11-12.
    var matcher = GetOption(options, "localeMatcher", "string", ["lookup", "best fit"], "best fit");
    opt.localeMatcher = matcher;

    // Step 13, unrolled.
    var numericValue = GetOption(options, "numeric", "boolean", undefined, undefined);
    if (numericValue !== undefined)
        numericValue = numericValue ? 'true' : 'false';
    opt.kn = numericValue;

    var caseFirstValue = GetOption(options, "caseFirst", "string", ["upper", "lower", "false"], undefined);
    opt.kf = caseFirstValue;

    // Compute remaining collation options.
    // Step 20.
    var s = GetOption(options, "sensitivity", "string",
                      ["base", "accent", "case", "variant"], undefined);
    lazyCollatorData.rawSensitivity = s;

    // Step 23.
    var ip = GetOption(options, "ignorePunctuation", "boolean", undefined, false);
    lazyCollatorData.ignorePunctuation = ip;

    // Step 26.
    //
    // We've done everything that must be done now: mark the lazy data as fully
    // computed and install it.
    setLazyData(internals, "Collator", lazyCollatorData);
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

    var availableLocales = collatorInternalProperties.availableLocales();
    var requestedLocales = CanonicalizeLocaleList(locales);
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
    availableLocales: function()
    {
        var locales = this._availableLocales;
        if (locales)
            return locales;
        return (this._availableLocales =
          addOldStyleLanguageTags(intl_Collator_availableLocales()));
    },
    relevantExtensionKeys: ["co", "kn"]
};


function collatorSortLocaleData(locale) {
    var collations = intl_availableCollations(locale);
    callFunction(std_Array_unshift, collations, null);
    return {
        co: collations,
        kn: ["false", "true"]
    };
}


function collatorSearchLocaleData(locale) {
    return {
        co: [null],
        kn: ["false", "true"],
        // In theory the default sensitivity is locale dependent;
        // in reality the CLDR/ICU default strength is always tertiary.
        sensitivity: "variant"
    };
}


/**
 * Function to be bound and returned by Intl.Collator.prototype.format.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.3.2.
 */
function collatorCompareToBind(x, y) {
    // Steps 1.a.i-ii implemented by ECMAScript declaration binding instantiation,
    // ES5.1 10.5, step 4.d.ii.

    // Step 1.a.iii-v.
    var X = ToString(x);
    var Y = ToString(y);
    return intl_CompareStrings(this, X, Y);
}


/**
 * Returns a function bound to this Collator that compares x (converted to a
 * String value) and y (converted to a String value),
 * and returns a number less than 0 if x < y, 0 if x = y, or a number greater
 * than 0 if x > y according to the sort order for the locale and collation
 * options of this Collator object.
 *
 * Spec: ECMAScript Internationalization API Specification, 10.3.2.
 */
function Intl_Collator_compare_get() {
    // Check "this Collator object" per introduction of section 10.3.
    var internals = getCollatorInternals(this, "compare");

    // Step 1.
    if (internals.boundCompare === undefined) {
        // Step 1.a.
        var F = collatorCompareToBind;

        // Step 1.b-d.
        var bc = callFunction(std_Function_bind, F, this);
        internals.boundCompare = bc;
    }

    // Step 2.
    return internals.boundCompare;
}


/**
 * Returns the resolved options for a Collator object.
 *
 * Spec: ECMAScript Internationalization API Specification, 10.3.3 and 10.4.
 */
function Intl_Collator_resolvedOptions() {
    // Check "this Collator object" per introduction of section 10.3.
    var internals = getCollatorInternals(this, "resolvedOptions");

    var result = {
        locale: internals.locale,
        usage: internals.usage,
        sensitivity: internals.sensitivity,
        ignorePunctuation: internals.ignorePunctuation
    };

    var relevantExtensionKeys = collatorInternalProperties.relevantExtensionKeys;
    for (var i = 0; i < relevantExtensionKeys.length; i++) {
        var key = relevantExtensionKeys[i];
        var property = (key === "co") ? "collation" : collatorKeyMappings[key].property;
        defineProperty(result, property, internals[property]);
    }
    return result;
}


/********** Intl.NumberFormat **********/


/**
 * NumberFormat internal properties.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.1 and 11.2.3.
 */
var numberFormatInternalProperties = {
    localeData: numberFormatLocaleData,
    _availableLocales: null,
    availableLocales: function()
    {
        var locales = this._availableLocales;
        if (locales)
            return locales;
        return (this._availableLocales =
          addOldStyleLanguageTags(intl_NumberFormat_availableLocales()));
    },
    relevantExtensionKeys: ["nu"]
};


/**
 * Compute an internal properties object from |lazyNumberFormatData|.
 */
function resolveNumberFormatInternals(lazyNumberFormatData) {
    assert(IsObject(lazyNumberFormatData), "lazy data not an object?");

    var internalProps = std_Object_create(null);

    // Step 3.
    var requestedLocales = lazyNumberFormatData.requestedLocales;

    // Compute options that impact interpretation of locale.
    // Step 6.
    var opt = lazyNumberFormatData.opt;

    // Compute effective locale.
    // Step 9.
    var NumberFormat = numberFormatInternalProperties;

    // Step 10.
    var localeData = NumberFormat.localeData;

    // Step 11.
    var r = ResolveLocale(NumberFormat.availableLocales(),
                          lazyNumberFormatData.requestedLocales,
                          lazyNumberFormatData.opt,
                          NumberFormat.relevantExtensionKeys,
                          localeData);

    // Steps 12-13.  (Step 14 is not relevant to our implementation.)
    internalProps.locale = r.locale;
    internalProps.numberingSystem = r.nu;

    // Compute formatting options.
    // Step 16.
    var s = lazyNumberFormatData.style;
    internalProps.style = s;

    // Steps 20, 22.
    if (s === "currency") {
        internalProps.currency = lazyNumberFormatData.currency;
        internalProps.currencyDisplay = lazyNumberFormatData.currencyDisplay;
    }

    // Step 24.
    internalProps.minimumIntegerDigits = lazyNumberFormatData.minimumIntegerDigits;

    // Steps 27.
    internalProps.minimumFractionDigits = lazyNumberFormatData.minimumFractionDigits;

    // Step 30.
    internalProps.maximumFractionDigits = lazyNumberFormatData.maximumFractionDigits;

    // Step 33.
    if ("minimumSignificantDigits" in lazyNumberFormatData) {
        // Note: Intl.NumberFormat.prototype.resolvedOptions() exposes the
        // actual presence (versus undefined-ness) of these properties.
        assert("maximumSignificantDigits" in lazyNumberFormatData, "min/max sig digits mismatch");
        internalProps.minimumSignificantDigits = lazyNumberFormatData.minimumSignificantDigits;
        internalProps.maximumSignificantDigits = lazyNumberFormatData.maximumSignificantDigits;
    }

    // Step 35.
    internalProps.useGrouping = lazyNumberFormatData.useGrouping;

    // Step 42.
    internalProps.boundFormat = undefined;

    // The caller is responsible for associating |internalProps| with the right
    // object using |setInternalProperties|.
    return internalProps;
}


/**
 * Returns an object containing the NumberFormat internal properties of |obj|,
 * or throws a TypeError if |obj| isn't NumberFormat-initialized.
 */
function getNumberFormatInternals(obj, methodName) {
    var internals = getIntlObjectInternals(obj, "NumberFormat", methodName);
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
 * Initializes an object as a NumberFormat.
 *
 * This method is complicated a moderate bit by its implementing initialization
 * as a *lazy* concept.  Everything that must happen now, does -- but we defer
 * all the work we can until the object is actually used as a NumberFormat.
 * This later work occurs in |resolveNumberFormatInternals|; steps not noted
 * here occur there.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.1.1.
 */
function InitializeNumberFormat(numberFormat, locales, options) {
    assert(IsObject(numberFormat), "InitializeNumberFormat");

    // Step 1.
    if (isInitializedIntlObject(numberFormat))
        ThrowError(JSMSG_INTL_OBJECT_REINITED);

    // Step 2.
    var internals = initializeIntlObject(numberFormat);

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
    // so every Collator lazy data object has *all* these properties, never a
    // subset of them.
    var lazyNumberFormatData = std_Object_create(null);

    // Step 3.
    var requestedLocales = CanonicalizeLocaleList(locales);
    lazyNumberFormatData.requestedLocales = requestedLocales;

    // Steps 4-5.
    //
    // If we ever need more speed here at startup, we should try to detect the
    // case where |options === undefined| and Object.prototype hasn't been
    // mucked with.  (|options| is fully consumed in this method, so it's not a
    // concern that Object.prototype might be touched between now and when
    // |resolveNumberFormatInternals| is called.)  For now just keep it simple.
    if (options === undefined)
        options = {};
    else
        options = ToObject(options);

    // Compute options that impact interpretation of locale.
    // Step 6.
    var opt = new Record();
    lazyNumberFormatData.opt = opt;

    // Steps 7-8.
    var matcher = GetOption(options, "localeMatcher", "string", ["lookup", "best fit"], "best fit");
    opt.localeMatcher = matcher;

    // Compute formatting options.
    // Step 15.
    var s = GetOption(options, "style", "string", ["decimal", "percent", "currency"], "decimal");
    lazyNumberFormatData.style = s;

    // Steps 17-20.
    var c = GetOption(options, "currency", "string", undefined, undefined);
    if (c !== undefined && !IsWellFormedCurrencyCode(c))
        ThrowError(JSMSG_INVALID_CURRENCY_CODE, c);
    var cDigits;
    if (s === "currency") {
        if (c === undefined)
            ThrowError(JSMSG_UNDEFINED_CURRENCY);

        // Steps 20.a-c.
        c = toASCIIUpperCase(c);
        lazyNumberFormatData.currency = c;
        cDigits = CurrencyDigits(c);
    }

    // Step 21.
    var cd = GetOption(options, "currencyDisplay", "string", ["code", "symbol", "name"], "symbol");
    if (s === "currency")
        lazyNumberFormatData.currencyDisplay = cd;

    // Step 23.
    var mnid = GetNumberOption(options, "minimumIntegerDigits", 1, 21, 1);
    lazyNumberFormatData.minimumIntegerDigits = mnid;

    // Steps 25-26.
    var mnfdDefault = (s === "currency") ? cDigits : 0;
    var mnfd = GetNumberOption(options, "minimumFractionDigits", 0, 20, mnfdDefault);
    lazyNumberFormatData.minimumFractionDigits = mnfd;

    // Steps 28-29.
    var mxfdDefault;
    if (s === "currency")
        mxfdDefault = std_Math_max(mnfd, cDigits);
    else if (s === "percent")
        mxfdDefault = std_Math_max(mnfd, 0);
    else
        mxfdDefault = std_Math_max(mnfd, 3);
    var mxfd = GetNumberOption(options, "maximumFractionDigits", mnfd, 20, mxfdDefault);
    lazyNumberFormatData.maximumFractionDigits = mxfd;

    // Steps 31-32.
    var mnsd = options.minimumSignificantDigits;
    var mxsd = options.maximumSignificantDigits;

    // Step 33.
    if (mnsd !== undefined || mxsd !== undefined) {
        mnsd = GetNumberOption(options, "minimumSignificantDigits", 1, 21, 1);
        mxsd = GetNumberOption(options, "maximumSignificantDigits", mnsd, 21, 21);
        lazyNumberFormatData.minimumSignificantDigits = mnsd;
        lazyNumberFormatData.maximumSignificantDigits = mxsd;
    }

    // Step 34.
    var g = GetOption(options, "useGrouping", "boolean", undefined, true);
    lazyNumberFormatData.useGrouping = g;

    // Step 43.
    //
    // We've done everything that must be done now: mark the lazy data as fully
    // computed and install it.
    setLazyData(internals, "NumberFormat", lazyNumberFormatData);
}


/**
 * Mapping from currency codes to the number of decimal digits used for them.
 * Default is 2 digits.
 *
 * Spec: ISO 4217 Currency and Funds Code List.
 * http://www.currency-iso.org/en/home/tables/table-a1.html
 */
var currencyDigits = {
    BHD: 3,
    BIF: 0,
    BYR: 0,
    CLF: 0,
    CLP: 0,
    DJF: 0,
    IQD: 3,
    GNF: 0,
    ISK: 0,
    JOD: 3,
    JPY: 0,
    KMF: 0,
    KRW: 0,
    KWD: 3,
    LYD: 3,
    OMR: 3,
    PYG: 0,
    RWF: 0,
    TND: 3,
    UGX: 0,
    UYI: 0,
    VND: 0,
    VUV: 0,
    XAF: 0,
    XOF: 0,
    XPF: 0
};


/**
 * Returns the number of decimal digits to be used for the given currency.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.1.1.
 */
function getCurrencyDigitsRE() {
    return internalIntlRegExps.currencyDigitsRE ||
           (internalIntlRegExps.currencyDigitsRE =
            regexp_construct_no_statics("^[A-Z]{3}$"));
}
function CurrencyDigits(currency) {
    assert(typeof currency === "string", "CurrencyDigits");
    assert(regexp_test_no_statics(getCurrencyDigitsRE(), currency), "CurrencyDigits");

    if (callFunction(std_Object_hasOwnProperty, currencyDigits, currency))
        return currencyDigits[currency];
    return 2;
}


/**
 * Returns the subset of the given locale list for which this locale list has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.2.2.
 */
function Intl_NumberFormat_supportedLocalesOf(locales /*, options*/) {
    var options = arguments.length > 1 ? arguments[1] : undefined;

    var availableLocales = numberFormatInternalProperties.availableLocales();
    var requestedLocales = CanonicalizeLocaleList(locales);
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
    // those decimal numbering systems explicitly listed in table 2 of
    // the ECMAScript Internationalization API Specification, 11.3.2, which
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


function numberFormatLocaleData(locale) {
    return {
        nu: getNumberingSystems(locale)
    };
}


/**
 * Function to be bound and returned by Intl.NumberFormat.prototype.format.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.3.2.
 */
function numberFormatFormatToBind(value) {
    // Steps 1.a.i implemented by ECMAScript declaration binding instantiation,
    // ES5.1 10.5, step 4.d.ii.

    // Step 1.a.ii-iii.
    var x = ToNumber(value);
    return intl_FormatNumber(this, x);
}


/**
 * Returns a function bound to this NumberFormat that returns a String value
 * representing the result of calling ToNumber(value) according to the
 * effective locale and the formatting options of this NumberFormat.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.3.2.
 */
function Intl_NumberFormat_format_get() {
    // Check "this NumberFormat object" per introduction of section 11.3.
    var internals = getNumberFormatInternals(this, "format");

    // Step 1.
    if (internals.boundFormat === undefined) {
        // Step 1.a.
        var F = numberFormatFormatToBind;

        // Step 1.b-d.
        var bf = callFunction(std_Function_bind, F, this);
        internals.boundFormat = bf;
    }
    // Step 2.
    return internals.boundFormat;
}


/**
 * Returns the resolved options for a NumberFormat object.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.3.3 and 11.4.
 */
function Intl_NumberFormat_resolvedOptions() {
    // Check "this NumberFormat object" per introduction of section 11.3.
    var internals = getNumberFormatInternals(this, "resolvedOptions");

    var result = {
        locale: internals.locale,
        numberingSystem: internals.numberingSystem,
        style: internals.style,
        minimumIntegerDigits: internals.minimumIntegerDigits,
        minimumFractionDigits: internals.minimumFractionDigits,
        maximumFractionDigits: internals.maximumFractionDigits,
        useGrouping: internals.useGrouping
    };
    var optionalProperties = [
        "currency",
        "currencyDisplay",
        "minimumSignificantDigits",
        "maximumSignificantDigits"
    ];
    for (var i = 0; i < optionalProperties.length; i++) {
        var p = optionalProperties[i];
        if (callFunction(std_Object_hasOwnProperty, internals, p))
            defineProperty(result, p, internals[p]);
    }
    return result;
}


/********** Intl.DateTimeFormat **********/


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
    //         hour12: true / false,  // optional
    //       }
    //
    //     timeZone: undefined / "UTC",
    //
    //     formatOpt: // *second* opt computed in InitializeDateTimeFormat
    //       {
    //         // all the properties/values listed in Table 3
    //         // (weekday, era, year, month, day, &c.)
    //       }
    //
    //     formatMatcher: "basic" / "best fit",
    //   }
    //
    // Note that lazy data is only installed as a final step of initialization,
    // so every DateTimeFormat lazy data object has *all* these properties,
    // never a subset of them.

    var internalProps = std_Object_create(null);

    // Compute effective locale.
    // Step 8.
    var DateTimeFormat = dateTimeFormatInternalProperties;

    // Step 9.
    var localeData = DateTimeFormat.localeData;

    // Step 10.
    var r = ResolveLocale(DateTimeFormat.availableLocales(),
                          lazyDateTimeFormatData.requestedLocales,
                          lazyDateTimeFormatData.localeOpt,
                          DateTimeFormat.relevantExtensionKeys,
                          localeData);

    // Steps 11-13.
    internalProps.locale = r.locale;
    internalProps.calendar = r.ca;
    internalProps.numberingSystem = r.nu;

    // Compute formatting options.
    // Step 14.
    var dataLocale = r.dataLocale;

    // Steps 15-17.
    internalProps.timeZone = lazyDateTimeFormatData.timeZone;

    // Step 18.
    var formatOpt = lazyDateTimeFormatData.formatOpt;

    // Steps 27-28, more or less - see comment after this function.
    var pattern = toBestICUPattern(dataLocale, formatOpt);

    // Step 29.
    internalProps.pattern = pattern;

    // Step 30.
    internalProps.boundFormat = undefined;

    // The caller is responsible for associating |internalProps| with the right
    // object using |setInternalProperties|.
    return internalProps;
}


/**
 * Returns an object containing the DateTimeFormat internal properties of |obj|,
 * or throws a TypeError if |obj| isn't DateTimeFormat-initialized.
 */
function getDateTimeFormatInternals(obj, methodName) {
    var internals = getIntlObjectInternals(obj, "DateTimeFormat", methodName);
    assert(internals.type === "DateTimeFormat", "bad type escaped getIntlObjectInternals");

    // If internal properties have already been computed, use them.
    var internalProps = maybeInternalProperties(internals);
    if (internalProps)
        return internalProps;

    // Otherwise it's time to fully create them.
    internalProps = resolveDateTimeFormatInternals(internals.lazyData);
    setInternalProperties(internals, internalProps);
    return internalProps;
}

/**
 * Components of date and time formats and their values.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.1.1.
 */
var dateTimeComponentValues = {
    weekday: ["narrow", "short", "long"],
    era: ["narrow", "short", "long"],
    year: ["2-digit", "numeric"],
    month: ["2-digit", "numeric", "narrow", "short", "long"],
    day: ["2-digit", "numeric"],
    hour: ["2-digit", "numeric"],
    minute: ["2-digit", "numeric"],
    second: ["2-digit", "numeric"],
    timeZoneName: ["short", "long"]
};


var dateTimeComponents = std_Object_getOwnPropertyNames(dateTimeComponentValues);


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
function InitializeDateTimeFormat(dateTimeFormat, locales, options) {
    assert(IsObject(dateTimeFormat), "InitializeDateTimeFormat");

    // Step 1.
    if (isInitializedIntlObject(dateTimeFormat))
        ThrowError(JSMSG_INTL_OBJECT_REINITED);

    // Step 2.
    var internals = initializeIntlObject(dateTimeFormat);

    // Lazy DateTimeFormat data has the following structure:
    //
    //   {
    //     requestedLocales: List of locales,
    //
    //     localeOpt: // *first* opt computed in InitializeDateTimeFormat
    //       {
    //         localeMatcher: "lookup" / "best fit",
    //       }
    //
    //     timeZone: undefined / "UTC",
    //
    //     formatOpt: // *second* opt computed in InitializeDateTimeFormat
    //       {
    //         // all the properties/values listed in Table 3
    //         // (weekday, era, year, month, day, &c.)
    //
    //         hour12: true / false  // optional
    //       }
    //
    //     formatMatcher: "basic" / "best fit",
    //   }
    //
    // Note that lazy data is only installed as a final step of initialization,
    // so every DateTimeFormat lazy data object has *all* these properties,
    // never a subset of them.
    var lazyDateTimeFormatData = std_Object_create(null);

    // Step 3.
    var requestedLocales = CanonicalizeLocaleList(locales);
    lazyDateTimeFormatData.requestedLocales = requestedLocales;

    // Step 4.
    options = ToDateTimeOptions(options, "any", "date");

    // Compute options that impact interpretation of locale.
    // Step 5.
    var localeOpt = new Record();
    lazyDateTimeFormatData.localeOpt = localeOpt;

    // Steps 6-7.
    var localeMatcher =
        GetOption(options, "localeMatcher", "string", ["lookup", "best fit"],
                  "best fit");
    localeOpt.localeMatcher = localeMatcher;

    // Steps 15-17.
    var tz = options.timeZone;
    if (tz !== undefined) {
        tz = toASCIIUpperCase(ToString(tz));
        if (tz !== "UTC")
            ThrowError(JSMSG_INVALID_TIME_ZONE, tz);
    }
    lazyDateTimeFormatData.timeZone = tz;

    // Step 18.
    var formatOpt = new Record();
    lazyDateTimeFormatData.formatOpt = formatOpt;

    // Step 19.
    var i, prop;
    for (i = 0; i < dateTimeComponents.length; i++) {
        prop = dateTimeComponents[i];
        var value = GetOption(options, prop, "string", dateTimeComponentValues[prop], undefined);
        formatOpt[prop] = value;
    }

    // Steps 20-21 provided by ICU - see comment after this function.

    // Step 22.
    //
    // For some reason (ICU not exposing enough interface?) we drop the
    // requested format matcher on the floor after this.  In any case, even if
    // doing so is justified, we have to do this work here in case it triggers
    // getters or similar.
    var formatMatcher =
        GetOption(options, "formatMatcher", "string", ["basic", "best fit"],
                  "best fit");

    // Steps 23-25 provided by ICU, more or less - see comment after this function.

    // Step 26.
    var hr12  = GetOption(options, "hour12", "boolean", undefined, undefined);

    // Pass hr12 on to ICU.
    if (hr12 !== undefined)
        formatOpt.hour12 = hr12;

    // Step 31.
    //
    // We've done everything that must be done now: mark the lazy data as fully
    // computed and install it.
    setLazyData(internals, "DateTimeFormat", lazyDateTimeFormatData);
}


// Intl.DateTimeFormat and ICU skeletons and patterns
// ==================================================
//
// Different locales have different ways to display dates using the same
// basic components. For example, en-US might use "Sept. 24, 2012" while
// fr-FR might use "24 Sept. 2012". The intent of Intl.DateTimeFormat is to
// permit production of a format for the locale that best matches the
// set of date-time components and their desired representation as specified
// by the API client.
//
// ICU supports specification of date and time formats in three ways:
//
// 1) A style is just one of the identifiers FULL, LONG, MEDIUM, or SHORT.
//    The date-time components included in each style and their representation
//    are defined by ICU using CLDR locale data (CLDR is the Unicode
//    Consortium's Common Locale Data Repository).
//
// 2) A skeleton is a string specifying which date-time components to include,
//    and which representations to use for them. For example, "yyyyMMMMdd"
//    specifies a year with at least four digits, a full month name, and a
//    two-digit day. It does not specify in which order the components appear,
//    how they are separated, the localized strings for textual components
//    (such as weekday or month), whether the month is in format or
//    stand-alone form¹, or the numbering system used for numeric components.
//    All that information is filled in by ICU using CLDR locale data.
//    ¹ The format form is the one used in formatted strings that include a
//    day; the stand-alone form is used when not including days, e.g., in
//    calendar headers. The two forms differ at least in some Slavic languages,
//    e.g. Russian: "22 марта 2013 г." vs. "Март 2013".
//
// 3) A pattern is a string specifying which date-time components to include,
//    in which order, with which separators, in which grammatical case. For
//    example, "EEEE, d MMMM y" specifies the full localized weekday name,
//    followed by comma and space, followed by the day, followed by space,
//    followed by the full month name in format form, followed by space,
//    followed by the full year. It
//    still does not specify localized strings for textual components and the
//    numbering system - these are determined by ICU using CLDR locale data or
//    possibly API parameters.
//
// All actual formatting in ICU is done with patterns; styles and skeletons
// have to be mapped to patterns before processing.
//
// The options of DateTimeFormat most closely correspond to ICU skeletons. This
// implementation therefore, in the toBestICUPattern function, converts
// DateTimeFormat options to ICU skeletons, and then lets ICU map skeletons to
// actual ICU patterns. The pattern may not directly correspond to what the
// skeleton requests, as the mapper (UDateTimePatternGenerator) is constrained
// by the available locale data for the locale. The resulting ICU pattern is
// kept as the DateTimeFormat's [[pattern]] internal property and passed to ICU
// in the format method.
//
// An ICU pattern represents the information of the following DateTimeFormat
// internal properties described in the specification, which therefore don't
// exist separately in the implementation:
// - [[weekday]], [[era]], [[year]], [[month]], [[day]], [[hour]], [[minute]],
//   [[second]], [[timeZoneName]]
// - [[hour12]]
// - [[hourNo0]]
// When needed for the resolvedOptions method, the resolveICUPattern function
// maps the instance's ICU pattern back to the specified properties of the
// object returned by resolvedOptions.
//
// ICU date-time skeletons and patterns aren't fully documented in the ICU
// documentation (see http://bugs.icu-project.org/trac/ticket/9627). The best
// documentation at this point is in UTR 35:
// http://unicode.org/reports/tr35/tr35-dates.html#Date_Format_Patterns


/**
 * Returns an ICU pattern string for the given locale and representing the
 * specified options as closely as possible given available locale data.
 */
function toBestICUPattern(locale, options) {
    // Create an ICU skeleton representing the specified options. See
    // http://unicode.org/reports/tr35/tr35-dates.html#Date_Field_Symbol_Table
    var skeleton = "";
    switch (options.weekday) {
    case "narrow":
        skeleton += "EEEEE";
        break;
    case "short":
        skeleton += "E";
        break;
    case "long":
        skeleton += "EEEE";
    }
    switch (options.era) {
    case "narrow":
        skeleton += "GGGGG";
        break;
    case "short":
        skeleton += "G";
        break;
    case "long":
        skeleton += "GGGG";
        break;
    }
    switch (options.year) {
    case "2-digit":
        skeleton += "yy";
        break;
    case "numeric":
        skeleton += "y";
        break;
    }
    switch (options.month) {
    case "2-digit":
        skeleton += "MM";
        break;
    case "numeric":
        skeleton += "M";
        break;
    case "narrow":
        skeleton += "MMMMM";
        break;
    case "short":
        skeleton += "MMM";
        break;
    case "long":
        skeleton += "MMMM";
        break;
    }
    switch (options.day) {
    case "2-digit":
        skeleton += "dd";
        break;
    case "numeric":
        skeleton += "d";
        break;
    }
    var hourSkeletonChar = "j";
    if (options.hour12 !== undefined) {
        if (options.hour12)
            hourSkeletonChar = "h";
        else
            hourSkeletonChar = "H";
    }
    switch (options.hour) {
    case "2-digit":
        skeleton += hourSkeletonChar + hourSkeletonChar;
        break;
    case "numeric":
        skeleton += hourSkeletonChar;
        break;
    }
    switch (options.minute) {
    case "2-digit":
        skeleton += "mm";
        break;
    case "numeric":
        skeleton += "m";
        break;
    }
    switch (options.second) {
    case "2-digit":
        skeleton += "ss";
        break;
    case "numeric":
        skeleton += "s";
        break;
    }
    switch (options.timeZoneName) {
    case "short":
        skeleton += "z";
        break;
    case "long":
        skeleton += "zzzz";
        break;
    }

    // Let ICU convert the ICU skeleton to an ICU pattern for the given locale.
    return intl_patternForSkeleton(locale, skeleton);
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

    // Steps 1-3.
    if (options === undefined)
        options = null;
    else
        options = ToObject(options);
    options = std_Object_create(options);

    // Step 4.
    var needDefaults = true;

    // Step 5.
    if ((required === "date" || required === "any") &&
        (options.weekday !== undefined || options.year !== undefined ||
         options.month !== undefined || options.day !== undefined))
    {
        needDefaults = false;
    }

    // Step 6.
    if ((required === "time" || required === "any") &&
        (options.hour !== undefined || options.minute !== undefined ||
         options.second !== undefined))
    {
        needDefaults = false;
    }

    // Step 7.
    if (needDefaults && (defaults === "date" || defaults === "all")) {
        // The specification says to call [[DefineOwnProperty]] with false for
        // the Throw parameter, while Object.defineProperty uses true. For the
        // calls here, the difference doesn't matter because we're adding
        // properties to a new object.
        defineProperty(options, "year", "numeric");
        defineProperty(options, "month", "numeric");
        defineProperty(options, "day", "numeric");
    }

    // Step 8.
    if (needDefaults && (defaults === "time" || defaults === "all")) {
        // See comment for step 7.
        defineProperty(options, "hour", "numeric");
        defineProperty(options, "minute", "numeric");
        defineProperty(options, "second", "numeric");
    }

    // Step 9.
    return options;
}


/**
 * Compares the date and time components requested by options with the available
 * date and time formats in formats, and selects the best match according
 * to a specified basic matching algorithm.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.1.1.
 */
function BasicFormatMatcher(options, formats) {
    // Steps 1-6.
    var removalPenalty = 120,
        additionPenalty = 20,
        longLessPenalty = 8,
        longMorePenalty = 6,
        shortLessPenalty = 6,
        shortMorePenalty = 3;

    // Table 3.
    var properties = ["weekday", "era", "year", "month", "day",
        "hour", "minute", "second", "timeZoneName"];

    // Step 11.c.vi.1.
    var values = ["2-digit", "numeric", "narrow", "short", "long"];

    // Steps 7-8.
    var bestScore = -Infinity;
    var bestFormat;

    // Steps 9-11.
    var i = 0;
    var len = formats.length;
    while (i < len) {
        // Steps 11.a-b.
        var format = formats[i];
        var score = 0;

        // Step 11.c.
        var formatProp;
        for (var j = 0; j < properties.length; j++) {
            var property = properties[j];

            // Step 11.c.i.
            var optionsProp = options[property];
            // Step missing from spec.
            // https://bugs.ecmascript.org/show_bug.cgi?id=1254
            formatProp = undefined;

            // Steps 11.c.ii-iii.
            if (callFunction(std_Object_hasOwnProperty, format, property))
                formatProp = format[property];

            if (optionsProp === undefined && formatProp !== undefined) {
                // Step 11.c.iv.
                score -= additionPenalty;
            } else if (optionsProp !== undefined && formatProp === undefined) {
                // Step 11.c.v.
                score -= removalPenalty;
            } else {
                // Step 11.c.vi.
                var optionsPropIndex = callFunction(std_Array_indexOf, values, optionsProp);
                var formatPropIndex = callFunction(std_Array_indexOf, values, formatProp);
                var delta = std_Math_max(std_Math_min(formatPropIndex - optionsPropIndex, 2), -2);
                if (delta === 2)
                    score -= longMorePenalty;
                else if (delta === 1)
                    score -= shortMorePenalty;
                else if (delta === -1)
                    score -= shortLessPenalty;
                else if (delta === -2)
                    score -= longLessPenalty;
            }
        }

        // Step 11.d.
        if (score > bestScore) {
            bestScore = score;
            bestFormat = format;
        }

        // Step 11.e.
        i++;
    }

    // Step 12.
    return bestFormat;
}


/**
 * Compares the date and time components requested by options with the available
 * date and time formats in formats, and selects the best match according
 * to an unspecified best-fit matching algorithm.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.1.1.
 */
function BestFitFormatMatcher(options, formats) {
    // this implementation doesn't have anything better
    return BasicFormatMatcher(options, formats);
}


/**
 * Returns the subset of the given locale list for which this locale list has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.2.2.
 */
function Intl_DateTimeFormat_supportedLocalesOf(locales /*, options*/) {
    var options = arguments.length > 1 ? arguments[1] : undefined;

    var availableLocales = dateTimeFormatInternalProperties.availableLocales();
    var requestedLocales = CanonicalizeLocaleList(locales);
    return SupportedLocales(availableLocales, requestedLocales, options);
}


/**
 * DateTimeFormat internal properties.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.1 and 12.2.3.
 */
var dateTimeFormatInternalProperties = {
    localeData: dateTimeFormatLocaleData,
    _availableLocales: null,
    availableLocales: function()
    {
        var locales = this._availableLocales;
        if (locales)
            return locales;
        return (this._availableLocales =
          addOldStyleLanguageTags(intl_DateTimeFormat_availableLocales()));
    },
    relevantExtensionKeys: ["ca", "nu"]
};


function dateTimeFormatLocaleData(locale) {
    return {
        ca: intl_availableCalendars(locale),
        nu: getNumberingSystems(locale)
    };
}


/**
 * Function to be bound and returned by Intl.DateTimeFormat.prototype.format.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.3.2.
 */
function dateTimeFormatFormatToBind() {
    // Steps 1.a.i-ii
    var date = arguments.length > 0 ? arguments[0] : undefined;
    var x = (date === undefined) ? std_Date_now() : ToNumber(date);

    // Step 1.a.iii.
    return intl_FormatDateTime(this, x);
}


/**
 * Returns a function bound to this DateTimeFormat that returns a String value
 * representing the result of calling ToNumber(date) according to the
 * effective locale and the formatting options of this DateTimeFormat.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.3.2.
 */
function Intl_DateTimeFormat_format_get() {
    // Check "this DateTimeFormat object" per introduction of section 12.3.
    var internals = getDateTimeFormatInternals(this, "format");

    // Step 1.
    if (internals.boundFormat === undefined) {
        // Step 1.a.
        var F = dateTimeFormatFormatToBind;

        // Step 1.b-d.
        var bf = callFunction(std_Function_bind, F, this);
        internals.boundFormat = bf;
    }

    // Step 2.
    return internals.boundFormat;
}


/**
 * Returns the resolved options for a DateTimeFormat object.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.3.3 and 12.4.
 */
function Intl_DateTimeFormat_resolvedOptions() {
    // Check "this DateTimeFormat object" per introduction of section 12.3.
    var internals = getDateTimeFormatInternals(this, "resolvedOptions");

    var result = {
        locale: internals.locale,
        calendar: internals.calendar,
        numberingSystem: internals.numberingSystem,
        timeZone: internals.timeZone
    };
    resolveICUPattern(internals.pattern, result);
    return result;
}


// Table mapping ICU pattern characters back to the corresponding date-time
// components of DateTimeFormat. See
// http://unicode.org/reports/tr35/tr35-dates.html#Date_Field_Symbol_Table
var icuPatternCharToComponent = {
    E: "weekday",
    G: "era",
    y: "year",
    M: "month",
    L: "month",
    d: "day",
    h: "hour",
    H: "hour",
    k: "hour",
    K: "hour",
    m: "minute",
    s: "second",
    z: "timeZoneName",
    v: "timeZoneName",
    V: "timeZoneName"
};


/**
 * Maps an ICU pattern string to a corresponding set of date-time components
 * and their values, and adds properties for these components to the result
 * object, which will be returned by the resolvedOptions method. For the
 * interpretation of ICU pattern characters, see
 * http://unicode.org/reports/tr35/tr35-dates.html#Date_Field_Symbol_Table
 */
function resolveICUPattern(pattern, result) {
    assert(IsObject(result), "resolveICUPattern");
    var i = 0;
    while (i < pattern.length) {
        var c = pattern[i++];
        if (c === "'") {
            while (i < pattern.length && pattern[i] !== "'")
                i++;
            i++;
        } else {
            var count = 1;
            while (i < pattern.length && pattern[i] === c) {
                i++;
                count++;
            }
            var value;
            switch (c) {
            // "text" cases
            case "G":
            case "E":
            case "z":
            case "v":
            case "V":
                if (count <= 3)
                    value = "short";
                else if (count === 4)
                    value = "long";
                else
                    value = "narrow";
                break;
            // "number" cases
            case "y":
            case "d":
            case "h":
            case "H":
            case "m":
            case "s":
            case "k":
            case "K":
                if (count === 2)
                    value = "2-digit";
                else
                    value = "numeric";
                break;
            // "text & number" cases
            case "M":
            case "L":
                if (count === 1)
                    value = "numeric";
                else if (count === 2)
                    value = "2-digit";
                else if (count === 3)
                    value = "short";
                else if (count === 4)
                    value = "long";
                else
                    value = "narrow";
                break;
            default:
                // skip other pattern characters and literal text
            }
            if (callFunction(std_Object_hasOwnProperty, icuPatternCharToComponent, c))
                defineProperty(result, icuPatternCharToComponent[c], value);
            if (c === "h" || c === "K")
                defineProperty(result, "hour12", true);
            else if (c === "H" || c === "k")
                defineProperty(result, "hour12", false);
        }
    }
}
