/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Portions Copyright Norbert Lindenberg 2011-2012. */

#ifdef DEBUG
#define JS_CONCAT2(x, y) x##y
#define JS_CONCAT(x, y) JS_CONCAT2(x, y)
#define assertIsValidAndCanonicalLanguageTag(locale, desc) \
  do { \
    var JS_CONCAT(canonical, __LINE__) = intl_TryValidateAndCanonicalizeLanguageTag(locale); \
    assert(JS_CONCAT(canonical, __LINE__) !== null, \
           `${desc} is a structurally valid language tag`); \
    assert(JS_CONCAT(canonical, __LINE__) === locale, \
           `${desc} is a canonicalized language tag`); \
  } while (false)
#else
#define assertIsValidAndCanonicalLanguageTag(locale, desc) ; // Elided assertion.
#endif

/**
 * Returns the start index of a "Unicode locale extension sequence", which the
 * specification defines as: "any substring of a language tag that starts with
 * a separator '-' and the singleton 'u' and includes the maximum sequence of
 * following non-singleton subtags and their preceding '-' separators."
 *
 * Alternatively, this may be defined as: the components of a language tag that
 * match the `unicode_locale_extensions` production in UTS 35.
 *
 * Spec: ECMAScript Internationalization API Specification, 6.2.1.
 */
function startOfUnicodeExtensions(locale) {
  assert(typeof locale === "string", "locale is a string");

  // Search for "-u-" marking the start of a Unicode extension sequence.
  var start = callFunction(std_String_indexOf, locale, "-u-");
  if (start < 0) {
    return -1;
  }

  // And search for "-x-" marking the start of any privateuse component to
  // handle the case when "-u-" was only found within a privateuse subtag.
  var privateExt = callFunction(std_String_indexOf, locale, "-x-");
  if (privateExt >= 0 && privateExt < start) {
    return -1;
  }

  return start;
}

/**
 * Returns the end index of a Unicode locale extension sequence.
 */
function endOfUnicodeExtensions(locale, start) {
  assert(typeof locale === "string", "locale is a string");
  assert(0 <= start && start < locale.length, "start is an index into locale");
  assert(
    Substring(locale, start, 3) === "-u-",
    "start points to Unicode extension sequence"
  );

  // Search for the start of the next singleton or privateuse subtag.
  //
  // Begin searching after the smallest possible Unicode locale extension
  // sequence, namely |"-u-" 2alphanum|. End searching once the remaining
  // characters can't fit the smallest possible singleton or privateuse
  // subtag, namely |"-x-" alphanum|. Note the reduced end-limit means
  // indexing inside the loop is always in-range.
  for (var i = start + 5, end = locale.length - 4; i <= end; i++) {
    if (locale[i] !== "-") {
      continue;
    }
    if (locale[i + 2] === "-") {
      return i;
    }

    // Skip over (i + 1) and (i + 2) because we've just verified they
    // aren't "-", so the next possible delimiter can only be at (i + 3).
    i += 2;
  }

  // If no singleton or privateuse subtag was found, the Unicode extension
  // sequence extends until the end of the string.
  return locale.length;
}

/**
 * Removes Unicode locale extension sequences from the given language tag.
 */
function removeUnicodeExtensions(locale) {
  assertIsValidAndCanonicalLanguageTag(
    locale,
    "locale with possible Unicode extension"
  );

  var start = startOfUnicodeExtensions(locale);
  if (start < 0) {
    return locale;
  }

  var end = endOfUnicodeExtensions(locale, start);

  var left = Substring(locale, 0, start);
  var right = Substring(locale, end, locale.length - end);
  var combined = left + right;

  assertIsValidAndCanonicalLanguageTag(combined, "the recombined locale");
  assert(
    startOfUnicodeExtensions(combined) < 0,
    "recombination failed to remove all Unicode locale extension sequences"
  );

  return combined;
}

/**
 * Returns Unicode locale extension sequences from the given language tag.
 */
function getUnicodeExtensions(locale) {
  assertIsValidAndCanonicalLanguageTag(locale, "locale with Unicode extension");

  var start = startOfUnicodeExtensions(locale);
  assert(start >= 0, "start of Unicode extension sequence not found");
  var end = endOfUnicodeExtensions(locale, start);

  return Substring(locale, start, end - start);
}

/**
 * Returns true if the input contains only ASCII alphabetical characters.
 */
function IsASCIIAlphaString(s) {
  assert(typeof s === "string", "IsASCIIAlphaString");

  for (var i = 0; i < s.length; i++) {
    var c = callFunction(std_String_charCodeAt, s, i);
    if (!((0x41 <= c && c <= 0x5a) || (0x61 <= c && c <= 0x7a))) {
      return false;
    }
  }
  return true;
}

var localeCache = {
  runtimeDefaultLocale: undefined,
  defaultLocale: undefined,
};

/**
 * Returns the BCP 47 language tag for the host environment's current locale.
 *
 * Spec: ECMAScript Internationalization API Specification, 6.2.4.
 */
function DefaultLocale() {
  if (intl_IsRuntimeDefaultLocale(localeCache.runtimeDefaultLocale)) {
    return localeCache.defaultLocale;
  }

  // If we didn't have a cache hit, compute the candidate default locale.
  var runtimeDefaultLocale = intl_RuntimeDefaultLocale();
  var locale = intl_supportedLocaleOrFallback(runtimeDefaultLocale);

  assertIsValidAndCanonicalLanguageTag(locale, "the computed default locale");
  assert(
    startOfUnicodeExtensions(locale) < 0,
    "the computed default locale must not contain a Unicode extension sequence"
  );

  // Cache the computed locale until the runtime default locale changes.
  localeCache.defaultLocale = locale;
  localeCache.runtimeDefaultLocale = runtimeDefaultLocale;

  return locale;
}

/**
 * Canonicalizes a locale list.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.1.
 */
function CanonicalizeLocaleList(locales) {
  // Step 1.
  if (locales === undefined) {
    return [];
  }

  // Step 3 (and the remaining steps).
  var tag = intl_ValidateAndCanonicalizeLanguageTag(locales, false);
  if (tag !== null) {
    assert(
      typeof tag === "string",
      "intl_ValidateAndCanonicalizeLanguageTag returns a string value"
    );
    return [tag];
  }

  // Step 2.
  var seen = [];

  // Step 4.
  var O = ToObject(locales);

  // Step 5.
  var len = ToLength(O.length);

  // Step 6.
  var k = 0;

  // Step 7.
  while (k < len) {
    // Steps 7.a-c.
    if (k in O) {
      // Step 7.c.i.
      var kValue = O[k];

      // Step 7.c.ii.
      if (!(typeof kValue === "string" || IsObject(kValue))) {
        ThrowTypeError(JSMSG_INVALID_LOCALES_ELEMENT);
      }

      // Steps 7.c.iii-iv.
      var tag = intl_ValidateAndCanonicalizeLanguageTag(kValue, true);
      assert(
        typeof tag === "string",
        "ValidateAndCanonicalizeLanguageTag returns a string value"
      );

      // Step 7.c.v.
      if (callFunction(std_Array_indexOf, seen, tag) === -1) {
        DefineDataProperty(seen, seen.length, tag);
      }
    }

    // Step 7.d.
    k++;
  }

  // Step 8.
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
  return intl_BestAvailableLocale(availableLocales, locale, DefaultLocale());
}

/**
 * Identical to BestAvailableLocale, but does not consider the default locale
 * during computation.
 */
function BestAvailableLocaleIgnoringDefault(availableLocales, locale) {
  return intl_BestAvailableLocale(availableLocales, locale, null);
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
  // Step 1.
  var result = new_Record();

  // Step 2.
  for (var i = 0; i < requestedLocales.length; i++) {
    var locale = requestedLocales[i];

    // Step 2.a.
    var noExtensionsLocale = removeUnicodeExtensions(locale);

    // Step 2.b.
    var availableLocale = BestAvailableLocale(
      availableLocales,
      noExtensionsLocale
    );

    // Step 2.c.
    if (availableLocale !== undefined) {
      // Step 2.c.i.
      result.locale = availableLocale;

      // Step 2.c.ii.
      if (locale !== noExtensionsLocale) {
        result.extension = getUnicodeExtensions(locale);
      }

      // Step 2.c.iii.
      return result;
    }
  }

  // Steps 3-4.
  result.locale = DefaultLocale();

  // Step 5.
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
 * Returns the Unicode extension value subtags for the requested key subtag.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.5.
 */
function UnicodeExtensionValue(extension, key) {
  assert(typeof extension === "string", "extension is a string value");
  assert(
    callFunction(std_String_startsWith, extension, "-u-") &&
      getUnicodeExtensions("und" + extension) === extension,
    "extension is a Unicode extension subtag"
  );
  assert(typeof key === "string", "key is a string value");

  // Step 1.
  assert(key.length === 2, "key is a Unicode extension key subtag");

  // Step 2.
  var size = extension.length;

  // Step 3.
  var searchValue = "-" + key + "-";

  // Step 4.
  var pos = callFunction(std_String_indexOf, extension, searchValue);

  // Step 5.
  if (pos !== -1) {
    // Step 5.a.
    var start = pos + 4;

    // Step 5.b.
    var end = start;

    // Step 5.c.
    var k = start;

    // Steps 5.d-e.
    while (true) {
      // Step 5.e.i.
      var e = callFunction(std_String_indexOf, extension, "-", k);

      // Step 5.e.ii.
      var len = e === -1 ? size - k : e - k;

      // Step 5.e.iii.
      if (len === 2) {
        break;
      }

      // Step 5.e.iv.
      if (e === -1) {
        end = size;
        break;
      }

      // Step 5.e.v.
      end = e;
      k = e + 1;
    }

    // Step 5.f.
    return callFunction(String_substring, extension, start, end);
  }

  // Step 6.
  searchValue = "-" + key;

  // Steps 7-8.
  if (callFunction(std_String_endsWith, extension, searchValue)) {
    return "";
  }

  // Step 9 (implicit).
}

/**
 * Compares a BCP 47 language priority list against availableLocales and
 * determines the best available language to meet the request. Options specified
 * through Unicode extension subsequences are negotiated separately, taking the
 * caller's relevant extensions and locale data as well as client-provided
 * options into consideration.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.6.
 */
function ResolveLocale(
  availableLocales,
  requestedLocales,
  options,
  relevantExtensionKeys,
  localeData
) {
  // Steps 1-3.
  var matcher = options.localeMatcher;
  var r =
    matcher === "lookup"
      ? LookupMatcher(availableLocales, requestedLocales)
      : BestFitMatcher(availableLocales, requestedLocales);

  // Step 4.
  var foundLocale = r.locale;
  var extension = r.extension;

  // Step 5.
  var result = new_Record();

  // Step 6.
  result.dataLocale = foundLocale;

  // Step 7.
  var supportedExtension = "-u";

  // In this implementation, localeData is a function, not an object.
  var localeDataProvider = localeData();

  // Step 8.
  for (var i = 0; i < relevantExtensionKeys.length; i++) {
    var key = relevantExtensionKeys[i];

    // Steps 8.a-h (The locale data is only computed when needed).
    var keyLocaleData = undefined;
    var value = undefined;

    // Locale tag may override.

    // Step 8.g.
    var supportedExtensionAddition = "";

    // Step 8.h.
    if (extension !== undefined) {
      // Step 8.h.i.
      var requestedValue = UnicodeExtensionValue(extension, key);

      // Step 8.h.ii.
      if (requestedValue !== undefined) {
        // Steps 8.a-d.
        keyLocaleData = callFunction(
          localeDataProvider[key],
          null,
          foundLocale
        );

        // Step 8.h.ii.1.
        if (requestedValue !== "") {
          // Step 8.h.ii.1.a.
          if (
            callFunction(std_Array_indexOf, keyLocaleData, requestedValue) !==
            -1
          ) {
            value = requestedValue;
            supportedExtensionAddition = "-" + key + "-" + value;
          }
        } else {
          // Step 8.h.ii.2.

          // According to the LDML spec, if there's no type value,
          // and true is an allowed value, it's used.
          if (callFunction(std_Array_indexOf, keyLocaleData, "true") !== -1) {
            value = "true";
            supportedExtensionAddition = "-" + key;
          }
        }
      }
    }

    // Options override all.

    // Step 8.i.i.
    var optionsValue = options[key];

    // Step 8.i.ii.
    assert(
      typeof optionsValue === "string" ||
        optionsValue === undefined ||
        optionsValue === null,
      "unexpected type for options value"
    );

    // Steps 8.i, 8.i.iii.1.
    if (optionsValue !== undefined && optionsValue !== value) {
      // Steps 8.a-d.
      if (keyLocaleData === undefined) {
        keyLocaleData = callFunction(
          localeDataProvider[key],
          null,
          foundLocale
        );
      }

      // Step 8.i.iii.
      if (callFunction(std_Array_indexOf, keyLocaleData, optionsValue) !== -1) {
        value = optionsValue;
        supportedExtensionAddition = "";
      }
    }

    // Locale data provides default value.
    if (value === undefined) {
      // Steps 8.a-f.
      value =
        keyLocaleData === undefined
          ? callFunction(localeDataProvider.default[key], null, foundLocale)
          : keyLocaleData[0];
    }

    // Step 8.j.
    assert(
      typeof value === "string" || value === null,
      "unexpected locale data value"
    );
    result[key] = value;

    // Step 8.k.
    supportedExtension += supportedExtensionAddition;
  }

  // Step 9.
  if (supportedExtension.length > 2) {
    foundLocale = addUnicodeExtension(foundLocale, supportedExtension);
  }

  // Step 10.
  result.locale = foundLocale;

  // Step 11.
  return result;
}

/**
 * Adds a Unicode extension subtag to a locale.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.6.
 */
function addUnicodeExtension(locale, extension) {
  assert(typeof locale === "string", "locale is a string value");
  assert(
    !callFunction(std_String_startsWith, locale, "x-"),
    "unexpected privateuse-only locale"
  );
  assert(
    startOfUnicodeExtensions(locale) < 0,
    "Unicode extension subtag already present in locale"
  );

  assert(typeof extension === "string", "extension is a string value");
  assert(
    callFunction(std_String_startsWith, extension, "-u-") &&
      getUnicodeExtensions("und" + extension) === extension,
    "extension is a Unicode extension subtag"
  );

  // Step 9.a.
  var privateIndex = callFunction(std_String_indexOf, locale, "-x-");

  // Steps 9.b-c.
  if (privateIndex === -1) {
    locale += extension;
  } else {
    var preExtension = callFunction(String_substring, locale, 0, privateIndex);
    var postExtension = callFunction(String_substring, locale, privateIndex);
    locale = preExtension + extension + postExtension;
  }

  // Steps 9.d-e (Step 9.e is not required in this implementation, because we don't canonicalize
  // Unicode extension subtags).
  assertIsValidAndCanonicalLanguageTag(locale, "locale after concatenation");

  return locale;
}

/**
 * Returns the subset of requestedLocales for which availableLocales has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.7.
 */
function LookupSupportedLocales(availableLocales, requestedLocales) {
  // Step 1.
  var subset = [];

  // Step 2.
  for (var i = 0; i < requestedLocales.length; i++) {
    var locale = requestedLocales[i];

    // Step 2.a.
    var noExtensionsLocale = removeUnicodeExtensions(locale);

    // Step 2.b.
    var availableLocale = BestAvailableLocale(
      availableLocales,
      noExtensionsLocale
    );

    // Step 2.c.
    if (availableLocale !== undefined) {
      DefineDataProperty(subset, subset.length, locale);
    }
  }

  // Step 3.
  return subset;
}

/**
 * Returns the subset of requestedLocales for which availableLocales has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.8.
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
 * Spec: ECMAScript Internationalization API Specification, 9.2.9.
 */
function SupportedLocales(availableLocales, requestedLocales, options) {
  // Step 1.
  var matcher;
  if (options !== undefined) {
    // Step 1.a.
    options = ToObject(options);

    // Step 1.b
    matcher = options.localeMatcher;
    if (matcher !== undefined) {
      matcher = ToString(matcher);
      if (matcher !== "lookup" && matcher !== "best fit") {
        ThrowRangeError(JSMSG_INVALID_LOCALE_MATCHER, matcher);
      }
    }
  }

  // Steps 2-5.
  return matcher === undefined || matcher === "best fit"
    ? BestFitSupportedLocales(availableLocales, requestedLocales)
    : LookupSupportedLocales(availableLocales, requestedLocales);
}

/**
 * Extracts a property value from the provided options object, converts it to
 * the required type, checks whether it is one of a list of allowed values,
 * and fills in a fallback value if necessary.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.10.
 */
function GetOption(options, property, type, values, fallback) {
  // Step 1.
  var value = options[property];

  // Step 2.
  if (value !== undefined) {
    // Steps 2.a-c.
    if (type === "boolean") {
      value = ToBoolean(value);
    } else if (type === "string") {
      value = ToString(value);
    } else {
      assert(false, "GetOption");
    }

    // Step 2.d.
    if (
      values !== undefined &&
      callFunction(std_Array_indexOf, values, value) === -1
    ) {
      ThrowRangeError(JSMSG_INVALID_OPTION_VALUE, property, `"${value}"`);
    }

    // Step 2.e.
    return value;
  }

  // Step 3.
  return fallback;
}

/**
 * Extracts a property value from the provided options object, converts it to
 * a boolean or string, checks whether it is one of a list of allowed values,
 * and fills in a fallback value if necessary.
 */
function GetStringOrBooleanOption(
  options,
  property,
  stringValues,
  fallback
) {
  assert(IsObject(stringValues), "GetStringOrBooleanOption");

  // Step 1.
  var value = options[property];

  // Step 2.
  if (value === undefined) {
    return fallback;
  }

  // Step 3.
  if (value === true) {
    return true;
  }

  // Steps 4-5.
  if (!value) {
    return false;
  }

  // Step 6.
  value = ToString(value);

  // Step 7.
  if (callFunction(std_Array_indexOf, stringValues, value) === -1) {
    ThrowRangeError(JSMSG_INVALID_OPTION_VALUE, property, `"${value}"`);
  }

  // Step 8.
  return value;
}

/**
 * The abstract operation DefaultNumberOption converts value to a Number value,
 * checks whether it is in the allowed range, and fills in a fallback value if
 * necessary.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.11.
 */
function DefaultNumberOption(value, minimum, maximum, fallback) {
  assert(
    typeof minimum === "number" && (minimum | 0) === minimum,
    "DefaultNumberOption"
  );
  assert(
    typeof maximum === "number" && (maximum | 0) === maximum,
    "DefaultNumberOption"
  );
  assert(
    fallback === undefined ||
      (typeof fallback === "number" && (fallback | 0) === fallback),
    "DefaultNumberOption"
  );
  assert(
    fallback === undefined || (minimum <= fallback && fallback <= maximum),
    "DefaultNumberOption"
  );

  // Step 1.
  if (value === undefined) {
    return fallback;
  }

  // Step 2.
  value = ToNumber(value);

  // Step 3.
  if (Number_isNaN(value) || value < minimum || value > maximum) {
    ThrowRangeError(JSMSG_INVALID_DIGITS_VALUE, value);
  }

  // Step 4.
  // Apply bitwise-or to convert -0 to +0 per ES2017, 5.2 and to ensure the
  // result is an int32 value.
  return std_Math_floor(value) | 0;
}

/**
 * Extracts a property value from the provided options object, converts it to a
 * Number value, checks whether it is in the allowed range, and fills in a
 * fallback value if necessary.
 *
 * Spec: ECMAScript Internationalization API Specification, 9.2.12.
 */
function GetNumberOption(options, property, minimum, maximum, fallback) {
  // Steps 1-2.
  return DefaultNumberOption(options[property], minimum, maximum, fallback);
}

// Symbols in the self-hosting compartment can't be cloned, use a separate
// object to hold the actual symbol value.
// TODO: Can we add support to clone symbols?
var intlFallbackSymbolHolder = { value: undefined };

/**
 * The [[FallbackSymbol]] symbol of the %Intl% intrinsic object.
 *
 * This symbol is used to implement the legacy constructor semantics for
 * Intl.DateTimeFormat and Intl.NumberFormat.
 */
function intlFallbackSymbol() {
  var fallbackSymbol = intlFallbackSymbolHolder.value;
  if (!fallbackSymbol) {
    var Symbol = GetBuiltinConstructor("Symbol");
    fallbackSymbol = Symbol("IntlLegacyConstructedSymbol");
    intlFallbackSymbolHolder.value = fallbackSymbol;
  }
  return fallbackSymbol;
}

/**
 * Initializes the INTL_INTERNALS_OBJECT_SLOT of the given object.
 */
function initializeIntlObject(obj, type, lazyData) {
  assert(IsObject(obj), "Non-object passed to initializeIntlObject");
  assert(
    (type === "Collator" && intl_GuardToCollator(obj) !== null) ||
      (type === "DateTimeFormat" && intl_GuardToDateTimeFormat(obj) !== null) ||
      (type === "DisplayNames" && intl_GuardToDisplayNames(obj) !== null) ||
      (type === "ListFormat" && intl_GuardToListFormat(obj) !== null) ||
      (type === "NumberFormat" && intl_GuardToNumberFormat(obj) !== null) ||
      (type === "PluralRules" && intl_GuardToPluralRules(obj) !== null) ||
      (type === "RelativeTimeFormat" &&
        intl_GuardToRelativeTimeFormat(obj) !== null) ||
      (type === "Segmenter" && intl_GuardToSegmenter(obj) !== null),
    "type must match the object's class"
  );
  assert(IsObject(lazyData), "non-object lazy data");

  // The meaning of an internals object for an object |obj| is as follows.
  //
  // The .type property indicates the type of Intl object that |obj| is. It
  // must be one of:
  // - Collator
  // - DateTimeFormat
  // - DisplayNames
  // - ListFormat
  // - NumberFormat
  // - PluralRules
  // - RelativeTimeFormat
  // - Segmenter
  //
  // The .lazyData property stores information needed to compute -- without
  // observable side effects -- the actual internal Intl properties of
  // |obj|.  If it is non-null, then the actual internal properties haven't
  // been computed, and .lazyData must be processed by
  // |setInternalProperties| before internal Intl property values are
  // available.  If it is null, then the .internalProps property contains an
  // object whose properties are the internal Intl properties of |obj|.

  var internals = std_Object_create(null);
  internals.type = type;
  internals.lazyData = lazyData;
  internals.internalProps = null;

  assert(
    UnsafeGetReservedSlot(obj, INTL_INTERNALS_OBJECT_SLOT) === undefined,
    "Internal slot already initialized?"
  );
  UnsafeSetReservedSlot(obj, INTL_INTERNALS_OBJECT_SLOT, internals);
}

/**
 * Set the internal properties object for an |internals| object previously
 * associated with lazy data.
 */
function setInternalProperties(internals, internalProps) {
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
function maybeInternalProperties(internals) {
  assert(IsObject(internals), "non-object passed to maybeInternalProperties");
  var lazyData = internals.lazyData;
  if (lazyData) {
    return null;
  }
  assert(
    IsObject(internals.internalProps),
    "missing lazy data and computed internals"
  );
  return internals.internalProps;
}

/**
 * Return |obj|'s internals object (*not* the object holding its internal
 * properties!), with structure specified above.
 *
 * Spec: ECMAScript Internationalization API Specification, 10.3.
 * Spec: ECMAScript Internationalization API Specification, 11.3.
 * Spec: ECMAScript Internationalization API Specification, 12.3.
 */
function getIntlObjectInternals(obj) {
  assert(IsObject(obj), "getIntlObjectInternals called with non-Object");
  assert(
    intl_GuardToCollator(obj) !== null ||
      intl_GuardToDateTimeFormat(obj) !== null ||
      intl_GuardToDisplayNames(obj) !== null ||
      intl_GuardToListFormat(obj) !== null ||
      intl_GuardToNumberFormat(obj) !== null ||
      intl_GuardToPluralRules(obj) !== null ||
      intl_GuardToRelativeTimeFormat(obj) !== null ||
      intl_GuardToSegmenter(obj) !== null,
    "getIntlObjectInternals called with non-Intl object"
  );

  var internals = UnsafeGetReservedSlot(obj, INTL_INTERNALS_OBJECT_SLOT);

  assert(IsObject(internals), "internals not an object");
  assert(hasOwn("type", internals), "missing type");
  assert(
    (internals.type === "Collator" && intl_GuardToCollator(obj) !== null) ||
      (internals.type === "DateTimeFormat" &&
        intl_GuardToDateTimeFormat(obj) !== null) ||
      (internals.type === "DisplayNames" &&
        intl_GuardToDisplayNames(obj) !== null) ||
      (internals.type === "ListFormat" &&
        intl_GuardToListFormat(obj) !== null) ||
      (internals.type === "NumberFormat" &&
        intl_GuardToNumberFormat(obj) !== null) ||
      (internals.type === "PluralRules" &&
        intl_GuardToPluralRules(obj) !== null) ||
      (internals.type === "RelativeTimeFormat" &&
        intl_GuardToRelativeTimeFormat(obj) !== null) ||
      (internals.type === "Segmenter" &&
        intl_GuardToSegmenter(obj) !== null),
    "type must match the object's class"
  );
  assert(hasOwn("lazyData", internals), "missing lazyData");
  assert(hasOwn("internalProps", internals), "missing internalProps");

  return internals;
}

/**
 * Get the internal properties of known-Intl object |obj|.  For use only by
 * C++ code that knows what it's doing!
 */
function getInternals(obj) {
  var internals = getIntlObjectInternals(obj);

  // If internal properties have already been computed, use them.
  var internalProps = maybeInternalProperties(internals);
  if (internalProps) {
    return internalProps;
  }

  // Otherwise it's time to fully create them.
  var type = internals.type;
  if (type === "Collator") {
    internalProps = resolveCollatorInternals(internals.lazyData);
  } else if (type === "DateTimeFormat") {
    internalProps = resolveDateTimeFormatInternals(internals.lazyData);
  } else if (type === "DisplayNames") {
    internalProps = resolveDisplayNamesInternals(internals.lazyData);
  } else if (type === "ListFormat") {
    internalProps = resolveListFormatInternals(internals.lazyData);
  } else if (type === "NumberFormat") {
    internalProps = resolveNumberFormatInternals(internals.lazyData);
  } else if (type === "PluralRules") {
    internalProps = resolvePluralRulesInternals(internals.lazyData);
  } else if (type === "RelativeTimeFormat") {
    internalProps = resolveRelativeTimeFormatInternals(internals.lazyData);
  } else {
    assert(type === "Segmenter", "unexpected Intl type");
    internalProps = resolveSegmenterInternals(internals.lazyData);
  }
  setInternalProperties(internals, internalProps);
  return internalProps;
}
