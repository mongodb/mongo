/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * ListFormat internal properties.
 */
function listFormatLocaleData() {
  // ListFormat don't support any extension keys.
  return {};
}
var listFormatInternalProperties = {
  localeData: listFormatLocaleData,
  relevantExtensionKeys: [],
};

/**
 * Intl.ListFormat ( [ locales [ , options ] ] )
 *
 * Compute an internal properties object from |lazyListFormatData|.
 */
function resolveListFormatInternals(lazyListFormatData) {
  assert(IsObject(lazyListFormatData), "lazy data not an object?");

  var internalProps = std_Object_create(null);

  var ListFormat = listFormatInternalProperties;

  // Compute effective locale.

  // Step 9.
  var localeData = ListFormat.localeData;

  // Step 10.
  var r = ResolveLocale(
    "ListFormat",
    lazyListFormatData.requestedLocales,
    lazyListFormatData.opt,
    ListFormat.relevantExtensionKeys,
    localeData
  );

  // Step 11.
  internalProps.locale = r.locale;

  // Step 13.
  internalProps.type = lazyListFormatData.type;

  // Step 15.
  internalProps.style = lazyListFormatData.style;

  // Steps 16-23 (not applicable in our implementation).

  // The caller is responsible for associating |internalProps| with the right
  // object using |setInternalProperties|.
  return internalProps;
}

/**
 * Returns an object containing the ListFormat internal properties of |obj|.
 */
function getListFormatInternals(obj) {
  assert(IsObject(obj), "getListFormatInternals called with non-object");
  assert(
    intl_GuardToListFormat(obj) !== null,
    "getListFormatInternals called with non-ListFormat"
  );

  var internals = getIntlObjectInternals(obj);
  assert(
    internals.type === "ListFormat",
    "bad type escaped getIntlObjectInternals"
  );

  // If internal properties have already been computed, use them.
  var internalProps = maybeInternalProperties(internals);
  if (internalProps) {
    return internalProps;
  }

  // Otherwise it's time to fully create them.
  internalProps = resolveListFormatInternals(internals.lazyData);
  setInternalProperties(internals, internalProps);
  return internalProps;
}

/**
 * Intl.ListFormat ( [ locales [ , options ] ] )
 *
 * Initializes an object as a ListFormat.
 *
 * This method is complicated a moderate bit by its implementing initialization
 * as a *lazy* concept.  Everything that must happen now, does -- but we defer
 * all the work we can until the object is actually used as a ListFormat.
 * This later work occurs in |resolveListFormatInternals|; steps not noted
 * here occur there.
 */
function InitializeListFormat(listFormat, locales, options) {
  assert(IsObject(listFormat), "InitializeListFormat called with non-object");
  assert(
    intl_GuardToListFormat(listFormat) !== null,
    "InitializeListFormat called with non-ListFormat"
  );

  // Lazy ListFormat data has the following structure:
  //
  //   {
  //     requestedLocales: List of locales,
  //     type: "conjunction" / "disjunction" / "unit",
  //     style: "long" / "short" / "narrow",
  //
  //     opt: // opt object computed in InitializeListFormat
  //       {
  //         localeMatcher: "lookup" / "best fit",
  //       }
  //   }
  //
  // Note that lazy data is only installed as a final step of initialization,
  // so every ListFormat lazy data object has *all* these properties, never a
  // subset of them.
  var lazyListFormatData = std_Object_create(null);

  // Step 3.
  var requestedLocales = CanonicalizeLocaleList(locales);
  lazyListFormatData.requestedLocales = requestedLocales;

  // Steps 4-5.
  if (options === undefined) {
    options = std_Object_create(null);
  } else if (!IsObject(options)) {
    ThrowTypeError(
      JSMSG_OBJECT_REQUIRED,
      options === null ? "null" : typeof options
    );
  }

  // Step 6.
  var opt = new_Record();
  lazyListFormatData.opt = opt;

  // Steps 7-8.
  var matcher = GetOption(
    options,
    "localeMatcher",
    "string",
    ["lookup", "best fit"],
    "best fit"
  );
  opt.localeMatcher = matcher;

  // Compute formatting options.

  // Steps 12-13.
  var type = GetOption(
    options,
    "type",
    "string",
    ["conjunction", "disjunction", "unit"],
    "conjunction"
  );
  lazyListFormatData.type = type;

  // Steps 14-15.
  var style = GetOption(
    options,
    "style",
    "string",
    ["long", "short", "narrow"],
    "long"
  );
  lazyListFormatData.style = style;

  // We've done everything that must be done now: mark the lazy data as fully
  // computed and install it.
  initializeIntlObject(listFormat, "ListFormat", lazyListFormatData);
}

/**
 * Returns the subset of the given locale list for which this locale list has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 */
function Intl_ListFormat_supportedLocalesOf(locales /*, options*/) {
  var options = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  // Step 1.
  var availableLocales = "ListFormat";

  // Step 2.
  var requestedLocales = CanonicalizeLocaleList(locales);

  // Step 3.
  return SupportedLocales(availableLocales, requestedLocales, options);
}

/**
 * StringListFromIterable ( iterable )
 */
function StringListFromIterable(iterable, methodName) {
  // Step 1.
  if (iterable === undefined) {
    return [];
  }

  // Step 3.
  var list = [];

  // Steps 2, 4-5.
  for (var element of allowContentIter(iterable)) {
    // Step 5.b.ii.
    if (typeof element !== "string") {
      ThrowTypeError(
        JSMSG_NOT_EXPECTED_TYPE,
        methodName,
        "string",
        typeof element
      );
    }

    // Step 5.b.iii.
    DefineDataProperty(list, list.length, element);
  }

  // Step 6.
  return list;
}

/**
 * Intl.ListFormat.prototype.format ( list )
 */
function Intl_ListFormat_format(list) {
  // Step 1.
  var listFormat = this;

  // Steps 2-3.
  if (
    !IsObject(listFormat) ||
    (listFormat = intl_GuardToListFormat(listFormat)) === null
  ) {
    return callFunction(
      intl_CallListFormatMethodIfWrapped,
      this,
      list,
      "Intl_ListFormat_format"
    );
  }

  // Step 4.
  var stringList = StringListFromIterable(list, "format");

  // We can directly return if |stringList| contains less than two elements.
  if (stringList.length < 2) {
    return stringList.length === 0 ? "" : stringList[0];
  }

  // Ensure the ListFormat internals are resolved.
  getListFormatInternals(listFormat);

  // Step 5.
  return intl_FormatList(listFormat, stringList, /* formatToParts = */ false);
}

/**
 * Intl.ListFormat.prototype.formatToParts ( list )
 */
function Intl_ListFormat_formatToParts(list) {
  // Step 1.
  var listFormat = this;

  // Steps 2-3.
  if (
    !IsObject(listFormat) ||
    (listFormat = intl_GuardToListFormat(listFormat)) === null
  ) {
    return callFunction(
      intl_CallListFormatMethodIfWrapped,
      this,
      list,
      "Intl_ListFormat_formatToParts"
    );
  }

  // Step 4.
  var stringList = StringListFromIterable(list, "formatToParts");

  // We can directly return if |stringList| contains less than two elements.
  if (stringList.length < 2) {
    return stringList.length === 0
      ? []
      : [{ type: "element", value: stringList[0] }];
  }

  // Ensure the ListFormat internals are resolved.
  getListFormatInternals(listFormat);

  // Step 5.
  return intl_FormatList(listFormat, stringList, /* formatToParts = */ true);
}

/**
 * Returns the resolved options for a ListFormat object.
 */
function Intl_ListFormat_resolvedOptions() {
  // Step 1.
  var listFormat = this;

  // Steps 2-3.
  if (
    !IsObject(listFormat) ||
    (listFormat = intl_GuardToListFormat(listFormat)) === null
  ) {
    return callFunction(
      intl_CallListFormatMethodIfWrapped,
      this,
      "Intl_ListFormat_resolvedOptions"
    );
  }

  var internals = getListFormatInternals(listFormat);

  // Steps 4-5.
  var result = {
    locale: internals.locale,
    type: internals.type,
    style: internals.style,
  };

  // Step 6.
  return result;
}
