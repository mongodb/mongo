/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Intl.Segmenter internal properties.
 */
function segmenterLocaleData() {
  // Segmenter doesn't support any extension keys.
  return {};
}
var segmenterInternalProperties = {
  localeData: segmenterLocaleData,
  relevantExtensionKeys: [],
};

/**
 * Intl.Segmenter ( [ locales [ , options ] ] )
 *
 * Compute an internal properties object from |lazySegmenterData|.
 */
function resolveSegmenterInternals(lazySegmenterData) {
  assert(IsObject(lazySegmenterData), "lazy data not an object?");

  var internalProps = std_Object_create(null);

  var Segmenter = segmenterInternalProperties;

  // Compute effective locale.

  // Step 9.
  var localeData = Segmenter.localeData;

  // Step 10.
  var r = ResolveLocale(
    "Segmenter",
    lazySegmenterData.requestedLocales,
    lazySegmenterData.opt,
    Segmenter.relevantExtensionKeys,
    localeData
  );

  // Step 11.
  internalProps.locale = r.locale;

  // Step 13.
  internalProps.granularity = lazySegmenterData.granularity;

  // The caller is responsible for associating |internalProps| with the right
  // object using |setInternalProperties|.
  return internalProps;
}

/**
 * Returns an object containing the Segmenter internal properties of |obj|.
 */
function getSegmenterInternals(obj) {
  assert(IsObject(obj), "getSegmenterInternals called with non-object");
  assert(
    intl_GuardToSegmenter(obj) !== null,
    "getSegmenterInternals called with non-Segmenter"
  );

  var internals = getIntlObjectInternals(obj);
  assert(
    internals.type === "Segmenter",
    "bad type escaped getIntlObjectInternals"
  );

  // If internal properties have already been computed, use them.
  var internalProps = maybeInternalProperties(internals);
  if (internalProps) {
    return internalProps;
  }

  // Otherwise it's time to fully create them.
  internalProps = resolveSegmenterInternals(internals.lazyData);
  setInternalProperties(internals, internalProps);
  return internalProps;
}

/**
 * Intl.Segmenter ( [ locales [ , options ] ] )
 *
 * Initializes an object as a Segmenter.
 *
 * This method is complicated a moderate bit by its implementing initialization
 * as a *lazy* concept.  Everything that must happen now, does -- but we defer
 * all the work we can until the object is actually used as a Segmenter.
 * This later work occurs in |resolveSegmenterInternals|; steps not noted here
 * occur there.
 */
function InitializeSegmenter(segmenter, locales, options) {
  assert(IsObject(segmenter), "InitializeSegmenter called with non-object");
  assert(
    intl_GuardToSegmenter(segmenter) !== null,
    "InitializeSegmenter called with non-Segmenter"
  );

  // Lazy Segmenter data has the following structure:
  //
  //   {
  //     requestedLocales: List of locales,
  //
  //     opt: // opt object computed in InitializeSegmenter
  //       {
  //         localeMatcher: "lookup" / "best fit",
  //       }
  //
  //     granularity: "grapheme" / "word" / "sentence",
  //   }
  //
  // Note that lazy data is only installed as a final step of initialization,
  // so every Segmenter lazy data object has *all* these properties, never a
  // subset of them.
  var lazySegmenterData = std_Object_create(null);

  // Step 4.
  var requestedLocales = CanonicalizeLocaleList(locales);
  lazySegmenterData.requestedLocales = requestedLocales;

  // Step 5.
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
  lazySegmenterData.opt = opt;

  // Steps 7-8.
  var matcher = GetOption(
    options,
    "localeMatcher",
    "string",
    ["lookup", "best fit"],
    "best fit"
  );
  opt.localeMatcher = matcher;

  // Steps 12-13.
  var granularity = GetOption(
    options,
    "granularity",
    "string",
    ["grapheme", "word", "sentence"],
    "grapheme"
  );
  lazySegmenterData.granularity = granularity;

  // We've done everything that must be done now: mark the lazy data as fully
  // computed and install it.
  initializeIntlObject(segmenter, "Segmenter", lazySegmenterData);
}

/**
 * Intl.Segmenter.supportedLocalesOf ( locales [, options ])
 *
 * Returns the subset of the given locale list for which this locale list has a
 * matching (possibly fallback) locale. Locales appear in the same order in the
 * returned list as in the input list.
 */
function Intl_Segmenter_supportedLocalesOf(locales /*, options*/) {
  var options = ArgumentsLength() > 1 ? GetArgument(1) : undefined;

  // Step 1.
  var availableLocales = "Segmenter";

  // Step 2.
  var requestedLocales = CanonicalizeLocaleList(locales);

  // Step 3.
  return SupportedLocales(availableLocales, requestedLocales, options);
}

/**
 * Intl.Segmenter.prototype.segment ( string )
 *
 * Create a new Segments object.
 */
function Intl_Segmenter_segment(value) {
  // Step 1.
  var segmenter = this;

  // Step 2.
  if (
    !IsObject(segmenter) ||
    (segmenter = intl_GuardToSegmenter(segmenter)) === null
  ) {
    return callFunction(
      intl_CallSegmenterMethodIfWrapped,
      this,
      value,
      "Intl_Segmenter_segment"
    );
  }

  // Ensure the Segmenter internals are resolved.
  getSegmenterInternals(segmenter);

  // Step 3.
  var string = ToString(value);

  // Step 4.
  return intl_CreateSegmentsObject(segmenter, string);
}

/**
 * Intl.Segmenter.prototype.resolvedOptions ()
 *
 * Returns the resolved options for a Segmenter object.
 */
function Intl_Segmenter_resolvedOptions() {
  // Step 1.
  var segmenter = this;

  // Step 2.
  if (
    !IsObject(segmenter) ||
    (segmenter = intl_GuardToSegmenter(segmenter)) === null
  ) {
    return callFunction(
      intl_CallSegmenterMethodIfWrapped,
      this,
      "Intl_Segmenter_resolvedOptions"
    );
  }

  var internals = getSegmenterInternals(segmenter);

  // Steps 3-4.
  var options = {
    locale: internals.locale,
    granularity: internals.granularity,
  };

  // Step 5.
  return options;
}

/**
 * CreateSegmentDataObject ( segmenter, string, startIndex, endIndex )
 */
function CreateSegmentDataObject(string, boundaries) {
  assert(typeof string === "string", "CreateSegmentDataObject");
  assert(
    IsPackedArray(boundaries) && boundaries.length === 3,
    "CreateSegmentDataObject"
  );

  var startIndex = boundaries[0];
  assert(
    typeof startIndex === "number" && (startIndex | 0) === startIndex,
    "startIndex is an int32-value"
  );

  var endIndex = boundaries[1];
  assert(
    typeof endIndex === "number" && (endIndex | 0) === endIndex,
    "endIndex is an int32-value"
  );

  // In our implementation |granularity| is encoded in |isWordLike|.
  var isWordLike = boundaries[2];
  assert(
    typeof isWordLike === "boolean" || isWordLike === undefined,
    "isWordLike is either a boolean or undefined"
  );

  // Step 1 (Not applicable).

  // Step 2.
  assert(startIndex >= 0, "startIndex is a positive number");

  // Step 3.
  assert(
    endIndex <= string.length,
    "endIndex is less-than-equals the string length"
  );

  // Step 4.
  assert(startIndex < endIndex, "startIndex is strictly less than endIndex");

  // Step 6.
  var segment = Substring(string, startIndex, endIndex - startIndex);

  // Steps 5, 7-12.
  if (isWordLike === undefined) {
    return {
      segment,
      index: startIndex,
      input: string,
    };
  }

  return {
    segment,
    index: startIndex,
    input: string,
    isWordLike,
  };
}

/**
 * %Segments.prototype%.containing ( index )
 *
 * Return a Segment Data object describing the segment at the given index. If
 * the index exceeds the string bounds, undefined is returned.
 */
function Intl_Segments_containing(index) {
  // Step 1.
  var segments = this;

  // Step 2.
  if (
    !IsObject(segments) ||
    (segments = intl_GuardToSegments(segments)) === null
  ) {
    return callFunction(
      intl_CallSegmentsMethodIfWrapped,
      this,
      index,
      "Intl_Segments_containing"
    );
  }

  // Step 3 (not applicable).

  // Step 4.
  var string = UnsafeGetStringFromReservedSlot(
    segments,
    INTL_SEGMENTS_STRING_SLOT
  );

  // Step 5.
  var len = string.length;

  // Step 6.
  var n = ToInteger(index);

  // Step 7.
  if (n < 0 || n >= len) {
    return undefined;
  }

  // Steps 8-9.
  var boundaries = intl_FindSegmentBoundaries(segments, n | 0);

  // Step 10.
  return CreateSegmentDataObject(string, boundaries);
}

/**
 * %Segments.prototype% [ @@iterator ] ()
 *
 * Create a new Segment Iterator object.
 */
function Intl_Segments_iterator() {
  // Step 1.
  var segments = this;

  // Step 2.
  if (
    !IsObject(segments) ||
    (segments = intl_GuardToSegments(segments)) === null
  ) {
    return callFunction(
      intl_CallSegmentsMethodIfWrapped,
      this,
      "Intl_Segments_iterator"
    );
  }

  // Steps 3-5.
  return intl_CreateSegmentIterator(segments);
}

/**
 * %SegmentIterator.prototype%.next ()
 *
 * Advance the Segment iterator to the next segment within the string.
 */
function Intl_SegmentIterator_next() {
  // Step 1.
  var iterator = this;

  // Step 2.
  if (
    !IsObject(iterator) ||
    (iterator = intl_GuardToSegmentIterator(iterator)) === null)
  {
    return callFunction(
      intl_CallSegmentIteratorMethodIfWrapped,
      this,
      "Intl_SegmentIterator_next"
    );
  }

  // Step 3 (Not applicable).

  // Step 4.
  var string = UnsafeGetStringFromReservedSlot(
    iterator,
    INTL_SEGMENT_ITERATOR_STRING_SLOT
  );

  // Step 5.
  var index = UnsafeGetInt32FromReservedSlot(
    iterator,
    INTL_SEGMENT_ITERATOR_INDEX_SLOT
  );

  var result = { value: undefined, done: false };

  // Step 7.
  if (index === string.length) {
    result.done = true;
    return result;
  }

  // Steps 6, 8.
  var boundaries = intl_FindNextSegmentBoundaries(iterator);

  // Step 9.
  result.value = CreateSegmentDataObject(string, boundaries);

  // Step 10.
  return result;
}
