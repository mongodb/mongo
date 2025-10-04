/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

function StringProtoHasNoMatch() {
  var ObjectProto = GetBuiltinPrototype("Object");
  var StringProto = GetBuiltinPrototype("String");
  if (!ObjectHasPrototype(StringProto, ObjectProto)) {
    return false;
  }
  return !(GetBuiltinSymbol("match") in StringProto);
}

function IsStringMatchOptimizable() {
  var RegExpProto = GetBuiltinPrototype("RegExp");
  // If RegExpPrototypeOptimizable succeeds, `exec` and `@@match` are
  // guaranteed to be data properties.
  return (
    RegExpPrototypeOptimizable(RegExpProto) &&
    RegExpProto.exec === RegExp_prototype_Exec &&
    RegExpProto[GetBuiltinSymbol("match")] === RegExpMatch
  );
}

function ThrowIncompatibleMethod(name, thisv) {
  ThrowTypeError(JSMSG_INCOMPATIBLE_PROTO, "String", name, ToString(thisv));
}

// ES 2016 draft Mar 25, 2016 21.1.3.11.
function String_match(regexp) {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("match", this);
  }

  // Step 2.
  var isPatternString = typeof regexp === "string";
  if (
    !(isPatternString && StringProtoHasNoMatch()) &&
    !IsNullOrUndefined(regexp)
  ) {
    // Step 2.a.
    var matcher = GetMethod(regexp, GetBuiltinSymbol("match"));

    // Step 2.b.
    if (matcher !== undefined) {
      return callContentFunction(matcher, regexp, this);
    }
  }

  // Step 3.
  var S = ToString(this);

  if (isPatternString && IsStringMatchOptimizable()) {
    var flatResult = FlatStringMatch(S, regexp);
    if (flatResult !== undefined) {
      return flatResult;
    }
  }

  // Step 4.
  var rx = RegExpCreate(regexp);

  // Step 5 (optimized case).
  if (IsStringMatchOptimizable()) {
    return RegExpMatcher(rx, S, 0);
  }

  // Step 5.
  return callContentFunction(GetMethod(rx, GetBuiltinSymbol("match")), rx, S);
}

// String.prototype.matchAll proposal.
//
// String.prototype.matchAll ( regexp )
function String_matchAll(regexp) {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("matchAll", this);
  }

  // Step 2.
  if (!IsNullOrUndefined(regexp)) {
    // Steps 2.a-b.
    if (IsRegExp(regexp)) {
      // Step 2.b.i.
      var flags = regexp.flags;

      // Step 2.b.ii.
      if (IsNullOrUndefined(flags)) {
        ThrowTypeError(JSMSG_FLAGS_UNDEFINED_OR_NULL);
      }

      // Step 2.b.iii.
      if (!callFunction(std_String_includes, ToString(flags), "g")) {
        ThrowTypeError(JSMSG_REQUIRES_GLOBAL_REGEXP, "matchAll");
      }
    }

    // Step 2.c.
    var matcher = GetMethod(regexp, GetBuiltinSymbol("matchAll"));

    // Step 2.d.
    if (matcher !== undefined) {
      return callContentFunction(matcher, regexp, this);
    }
  }

  // Step 3.
  var string = ToString(this);

  // Step 4.
  var rx = RegExpCreate(regexp, "g");

  // Step 5.
  return callContentFunction(
    GetMethod(rx, GetBuiltinSymbol("matchAll")),
    rx,
    string
  );
}

/**
 * A helper function implementing the logic for both String.prototype.padStart
 * and String.prototype.padEnd as described in ES7 Draft March 29, 2016
 */
function String_pad(maxLength, fillString, padEnd) {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod(padEnd ? "padEnd" : "padStart", this);
  }

  // Step 2.
  var str = ToString(this);

  // Steps 3-4.
  var intMaxLength = ToLength(maxLength);
  var strLen = str.length;

  // Step 5.
  if (intMaxLength <= strLen) {
    return str;
  }

  // Steps 6-7.
  assert(fillString !== undefined, "never called when fillString is undefined");
  var filler = ToString(fillString);

  // Step 8.
  if (filler === "") {
    return str;
  }

  // Throw an error if the final string length exceeds the maximum string
  // length. Perform this check early so we can use int32 operations below.
  if (intMaxLength > MAX_STRING_LENGTH) {
    ThrowRangeError(JSMSG_RESULTING_STRING_TOO_LARGE);
  }

  // Step 9.
  var fillLen = intMaxLength - strLen;

  // Step 10.
  // Perform an int32 division to ensure String_repeat is not called with a
  // double to avoid repeated bailouts in ToInteger.
  var truncatedStringFiller = callFunction(
    String_repeat,
    filler,
    (fillLen / filler.length) | 0
  );

  truncatedStringFiller += Substring(filler, 0, fillLen % filler.length);

  // Step 11.
  if (padEnd === true) {
    return str + truncatedStringFiller;
  }
  return truncatedStringFiller + str;
}

function String_pad_start(maxLength, fillString = " ") {
  return callFunction(String_pad, this, maxLength, fillString, false);
}

function String_pad_end(maxLength, fillString = " ") {
  return callFunction(String_pad, this, maxLength, fillString, true);
}

function StringProtoHasNoReplace() {
  var ObjectProto = GetBuiltinPrototype("Object");
  var StringProto = GetBuiltinPrototype("String");
  if (!ObjectHasPrototype(StringProto, ObjectProto)) {
    return false;
  }
  return !(GetBuiltinSymbol("replace") in StringProto);
}

// A thin wrapper to call SubstringKernel with int32-typed arguments.
// Caller should check the range of |from| and |length|.
function Substring(str, from, length) {
  assert(typeof str === "string", "|str| should be a string");
  assert(
    (from | 0) === from,
    "coercing |from| into int32 should not change the value"
  );
  assert(
    (length | 0) === length,
    "coercing |length| into int32 should not change the value"
  );

  return SubstringKernel(
    str,
    std_Math_max(from, 0) | 0,
    std_Math_max(length, 0) | 0
  );
}

// ES 2016 draft Mar 25, 2016 21.1.3.14.
function String_replace(searchValue, replaceValue) {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("replace", this);
  }

  // Step 2.
  if (
    !(typeof searchValue === "string" && StringProtoHasNoReplace()) &&
    !IsNullOrUndefined(searchValue)
  ) {
    // Step 2.a.
    var replacer = GetMethod(searchValue, GetBuiltinSymbol("replace"));

    // Step 2.b.
    if (replacer !== undefined) {
      return callContentFunction(replacer, searchValue, this, replaceValue);
    }
  }

  // Step 3.
  var string = ToString(this);

  // Step 4.
  var searchString = ToString(searchValue);

  if (typeof replaceValue === "string") {
    // Steps 6-12: Optimized for string case.
    return StringReplaceString(string, searchString, replaceValue);
  }

  // Step 5.
  if (!IsCallable(replaceValue)) {
    // Steps 6-12.
    return StringReplaceString(string, searchString, ToString(replaceValue));
  }

  // Step 7.
  var pos = callFunction(std_String_indexOf, string, searchString);
  if (pos === -1) {
    return string;
  }

  // Step 8.
  var replStr = ToString(
    callContentFunction(replaceValue, undefined, searchString, pos, string)
  );

  // Step 10.
  var tailPos = pos + searchString.length;

  // Step 11.
  var newString;
  if (pos === 0) {
    newString = "";
  } else {
    newString = Substring(string, 0, pos);
  }

  newString += replStr;
  var stringLength = string.length;
  if (tailPos < stringLength) {
    newString += Substring(string, tailPos, stringLength - tailPos);
  }

  // Step 12.
  return newString;
}

// String.prototype.replaceAll (Stage 3 proposal)
// https://tc39.es/proposal-string-replaceall/
//
// String.prototype.replaceAll ( searchValue, replaceValue )
function String_replaceAll(searchValue, replaceValue) {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("replaceAll", this);
  }

  // Step 2.
  if (!IsNullOrUndefined(searchValue)) {
    // Steps 2.a-b.
    if (IsRegExp(searchValue)) {
      // Step 2.b.i.
      var flags = searchValue.flags;

      // Step 2.b.ii.
      if (IsNullOrUndefined(flags)) {
        ThrowTypeError(JSMSG_FLAGS_UNDEFINED_OR_NULL);
      }

      // Step 2.b.iii.
      if (!callFunction(std_String_includes, ToString(flags), "g")) {
        ThrowTypeError(JSMSG_REQUIRES_GLOBAL_REGEXP, "replaceAll");
      }
    }

    // Step 2.c.
    var replacer = GetMethod(searchValue, GetBuiltinSymbol("replace"));

    // Step 2.b.
    if (replacer !== undefined) {
      return callContentFunction(replacer, searchValue, this, replaceValue);
    }
  }

  // Step 3.
  var string = ToString(this);

  // Step 4.
  var searchString = ToString(searchValue);

  // Steps 5-6.
  if (!IsCallable(replaceValue)) {
    // Steps 7-16.
    return StringReplaceAllString(string, searchString, ToString(replaceValue));
  }

  // Step 7.
  var searchLength = searchString.length;

  // Step 8.
  var advanceBy = std_Math_max(1, searchLength);

  // Step 9 (not needed in this implementation).

  // Step 12.
  var endOfLastMatch = 0;

  // Step 13.
  var result = "";

  // Steps 10-11, 14.
  var position = 0;
  while (true) {
    // Steps 10-11.
    //
    // StringIndexOf doesn't clamp the |position| argument to the input
    // string length, i.e. |StringIndexOf("abc", "", 4)| returns -1,
    // whereas |"abc".indexOf("", 4)| returns 3. That means we need to
    // exit the loop when |nextPosition| is smaller than |position| and
    // not just when |nextPosition| is -1.
    var nextPosition = callFunction(
      std_String_indexOf,
      string,
      searchString,
      position
    );
    if (nextPosition < position) {
      break;
    }
    position = nextPosition;

    // Step 14.a.
    var replacement = ToString(
      callContentFunction(
        replaceValue,
        undefined,
        searchString,
        position,
        string
      )
    );

    // Step 14.b (not applicable).

    // Step 14.c.
    var stringSlice = Substring(
      string,
      endOfLastMatch,
      position - endOfLastMatch
    );

    // Step 14.d.
    result += stringSlice + replacement;

    // Step 14.e.
    endOfLastMatch = position + searchLength;

    // Step 11.b.
    position += advanceBy;
  }

  // Step 15.
  if (endOfLastMatch < string.length) {
    // Step 15.a.
    result += Substring(string, endOfLastMatch, string.length - endOfLastMatch);
  }

  // Step 16.
  return result;
}

function StringProtoHasNoSearch() {
  var ObjectProto = GetBuiltinPrototype("Object");
  var StringProto = GetBuiltinPrototype("String");
  if (!ObjectHasPrototype(StringProto, ObjectProto)) {
    return false;
  }
  return !(GetBuiltinSymbol("search") in StringProto);
}

function IsStringSearchOptimizable() {
  var RegExpProto = GetBuiltinPrototype("RegExp");
  // If RegExpPrototypeOptimizable succeeds, `exec` and `@@search` are
  // guaranteed to be data properties.
  return (
    RegExpPrototypeOptimizable(RegExpProto) &&
    RegExpProto.exec === RegExp_prototype_Exec &&
    RegExpProto[GetBuiltinSymbol("search")] === RegExpSearch
  );
}

// ES 2016 draft Mar 25, 2016 21.1.3.15.
function String_search(regexp) {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("search", this);
  }

  // Step 2.
  var isPatternString = typeof regexp === "string";
  if (
    !(isPatternString && StringProtoHasNoSearch()) &&
    !IsNullOrUndefined(regexp)
  ) {
    // Step 2.a.
    var searcher = GetMethod(regexp, GetBuiltinSymbol("search"));

    // Step 2.b.
    if (searcher !== undefined) {
      return callContentFunction(searcher, regexp, this);
    }
  }

  // Step 3.
  var string = ToString(this);

  if (isPatternString && IsStringSearchOptimizable()) {
    var flatResult = FlatStringSearch(string, regexp);
    if (flatResult !== -2) {
      return flatResult;
    }
  }

  // Step 4.
  var rx = RegExpCreate(regexp);

  // Step 5.
  return callContentFunction(
    GetMethod(rx, GetBuiltinSymbol("search")),
    rx,
    string
  );
}

function StringProtoHasNoSplit() {
  var ObjectProto = GetBuiltinPrototype("Object");
  var StringProto = GetBuiltinPrototype("String");
  if (!ObjectHasPrototype(StringProto, ObjectProto)) {
    return false;
  }
  return !(GetBuiltinSymbol("split") in StringProto);
}

// ES 2016 draft Mar 25, 2016 21.1.3.17.
function String_split(separator, limit) {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("split", this);
  }

  // Optimized path for string.split(string), especially when both strings
  // are constants.  Following sequence of if's cannot be put together in
  // order that IonMonkey sees the constant if present (bug 1246141).
  if (typeof this === "string") {
    if (StringProtoHasNoSplit()) {
      if (typeof separator === "string") {
        if (limit === undefined) {
          // inlineConstantStringSplitString needs both arguments to
          // be MConstant, so pass them directly.
          return StringSplitString(this, separator);
        }
      }
    }
  }

  // Step 2.
  if (
    !(typeof separator === "string" && StringProtoHasNoSplit()) &&
    !IsNullOrUndefined(separator)
  ) {
    // Step 2.a.
    var splitter = GetMethod(separator, GetBuiltinSymbol("split"));

    // Step 2.b.
    if (splitter !== undefined) {
      return callContentFunction(splitter, separator, this, limit);
    }
  }

  // Step 3.
  var S = ToString(this);

  // Step 6.
  var R;
  if (limit !== undefined) {
    var lim = limit >>> 0;

    // Step 9.
    R = ToString(separator);

    // Step 10.
    if (lim === 0) {
      return [];
    }

    // Step 11.
    if (separator === undefined) {
      return [S];
    }

    // Steps 4, 8, 12-18.
    return StringSplitStringLimit(S, R, lim);
  }

  // Step 9.
  R = ToString(separator);

  // Step 11.
  if (separator === undefined) {
    return [S];
  }

  // Optimized path.
  // Steps 4, 8, 12-18.
  return StringSplitString(S, R);
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 21.1.3.22 String.prototype.substring ( start, end )
function String_substring(start, end) {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("substring", this);
  }

  // Step 2.
  var str = ToString(this);

  // Step 3.
  var len = str.length;

  // Step 4.
  var intStart = ToInteger(start);

  // Step 5.
  var intEnd = end === undefined ? len : ToInteger(end);

  // Step 6.
  var finalStart = std_Math_min(std_Math_max(intStart, 0), len);

  // Step 7.
  var finalEnd = std_Math_min(std_Math_max(intEnd, 0), len);

  // Step 8.
  var from = std_Math_min(finalStart, finalEnd);

  // Step 9.
  var to = std_Math_max(finalStart, finalEnd);

  // Step 10.
  // While |from| and |to - from| are bounded to the length of |str| and this
  // and thus definitely in the int32 range, they can still be typed as
  // double. Eagerly truncate since SubstringKernel only accepts int32.
  return SubstringKernel(str, from | 0, (to - from) | 0);
}
SetIsInlinableLargeFunction(String_substring);

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// B.2.3.1 String.prototype.substr ( start, length )
function String_substr(start, length) {
  // Steps 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("substr", this);
  }

  // Step 2.
  var str = ToString(this);

  // Step 3.
  var intStart = ToInteger(start);

  // Steps 4-5.
  var size = str.length;
  // Use |size| instead of +Infinity to avoid performing calculations with
  // doubles. (The result is the same either way.)
  var end = length === undefined ? size : ToInteger(length);

  // Step 6.
  if (intStart < 0) {
    intStart = std_Math_max(intStart + size, 0);
  } else {
    // Restrict the input range to allow better Ion optimizations.
    intStart = std_Math_min(intStart, size);
  }

  // Step 7.
  var resultLength = std_Math_min(std_Math_max(end, 0), size - intStart);

  // Step 8.
  assert(
    0 <= resultLength && resultLength <= size - intStart,
    "resultLength is a valid substring length value"
  );

  // Step 9.
  // While |intStart| and |resultLength| are bounded to the length of |str|
  // and thus definitely in the int32 range, they can still be typed as
  // double. Eagerly truncate since SubstringKernel only accepts int32.
  return SubstringKernel(str, intStart | 0, resultLength | 0);
}
SetIsInlinableLargeFunction(String_substr);

// ES2021 draft rev 12a546b92275a0e2f834017db2727bb9c6f6c8fd
// 21.1.3.4 String.prototype.concat ( ...args )
// Note: String.prototype.concat.length is 1.
function String_concat(arg1) {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("concat", this);
  }

  // Step 2.
  var str = ToString(this);

  // Specialize for the most common number of arguments for better inlining.
  if (ArgumentsLength() === 0) {
    return str;
  }
  if (ArgumentsLength() === 1) {
    return str + ToString(GetArgument(0));
  }
  if (ArgumentsLength() === 2) {
    return str + ToString(GetArgument(0)) + ToString(GetArgument(1));
  }

  // Step 3. (implicit)
  // Step 4.
  var result = str;

  // Step 5.
  for (var i = 0; i < ArgumentsLength(); i++) {
    // Steps 5.a-b.
    var nextString = ToString(GetArgument(i));
    // Step 5.c.
    result += nextString;
  }

  // Step 6.
  return result;
}

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 21.1.3.19 String.prototype.slice ( start, end )
function String_slice(start, end) {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("slice", this);
  }

  // Step 2.
  var str = ToString(this);

  // Step 3.
  var len = str.length;

  // Step 4.
  var intStart = ToInteger(start);

  // Step 5.
  var intEnd = end === undefined ? len : ToInteger(end);

  // Step 6.
  var from =
    intStart < 0
      ? std_Math_max(len + intStart, 0)
      : std_Math_min(intStart, len);

  // Step 7.
  var to =
    intEnd < 0 ? std_Math_max(len + intEnd, 0) : std_Math_min(intEnd, len);

  // Step 8.
  var span = std_Math_max(to - from, 0);

  // Step 9.
  // While |from| and |span| are bounded to the length of |str|
  // and thus definitely in the int32 range, they can still be typed as
  // double. Eagerly truncate since SubstringKernel only accepts int32.
  return SubstringKernel(str, from | 0, span | 0);
}
SetIsInlinableLargeFunction(String_slice);

// ES2020 draft rev dc1e21c454bd316810be1c0e7af0131a2d7f38e9
// 21.1.3.16 String.prototype.repeat ( count )
function String_repeat(count) {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("repeat", this);
  }

  // Step 2.
  var S = ToString(this);

  // Step 3.
  var n = ToInteger(count);

  // Step 4.
  if (n < 0) {
    ThrowRangeError(JSMSG_NEGATIVE_REPETITION_COUNT);
  }

  // Step 5.
  // Inverted condition to handle |Infinity * 0 = NaN| correctly.
  if (!(n * S.length <= MAX_STRING_LENGTH)) {
    ThrowRangeError(JSMSG_RESULTING_STRING_TOO_LARGE);
  }

  // Communicate |n|'s possible range to the compiler. We actually use
  // MAX_STRING_LENGTH + 1 as range because that's a valid bit mask. That's
  // fine because it's only used as optimization hint.
  assert(
    TO_INT32(MAX_STRING_LENGTH + 1) === MAX_STRING_LENGTH + 1,
    "MAX_STRING_LENGTH + 1 must fit in int32"
  );
  assert(
    ((MAX_STRING_LENGTH + 1) & (MAX_STRING_LENGTH + 2)) === 0,
    "MAX_STRING_LENGTH + 1 can be used as a bitmask"
  );
  n = n & (MAX_STRING_LENGTH + 1);

  // Steps 6-7.
  var T = "";
  for (;;) {
    if (n & 1) {
      T += S;
    }
    n >>= 1;
    if (n) {
      S += S;
    } else {
      break;
    }
  }
  return T;
}

// ES6 draft specification, section 21.1.3.27, version 2013-09-27.
function String_iterator() {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowTypeError(
      JSMSG_INCOMPATIBLE_PROTO2,
      "String",
      "Symbol.iterator",
      ToString(this)
    );
  }

  // Step 2.
  var S = ToString(this);

  // Step 3.
  var iterator = NewStringIterator();
  UnsafeSetReservedSlot(iterator, ITERATOR_SLOT_TARGET, S);
  UnsafeSetReservedSlot(iterator, ITERATOR_SLOT_NEXT_INDEX, 0);
  return iterator;
}

function StringIteratorNext() {
  var obj = this;
  if (!IsObject(obj) || (obj = GuardToStringIterator(obj)) === null) {
    return callFunction(
      CallStringIteratorMethodIfWrapped,
      this,
      "StringIteratorNext"
    );
  }

  var S = UnsafeGetStringFromReservedSlot(obj, ITERATOR_SLOT_TARGET);
  // We know that JSString::MAX_LENGTH <= INT32_MAX (and assert this in
  // SelfHostring.cpp) so our current index can never be anything other than
  // an Int32Value.
  var index = UnsafeGetInt32FromReservedSlot(obj, ITERATOR_SLOT_NEXT_INDEX);
  var size = S.length;
  var result = { value: undefined, done: false };

  if (index >= size) {
    result.done = true;
    return result;
  }

  var codePoint = callFunction(std_String_codePointAt, S, index);
  var charCount = 1 + (codePoint > 0xffff);

  UnsafeSetReservedSlot(obj, ITERATOR_SLOT_NEXT_INDEX, index + charCount);

  result.value = callFunction(std_String_fromCodePoint, null, codePoint);

  return result;
}
SetIsInlinableLargeFunction(StringIteratorNext);

#if JS_HAS_INTL_API
var collatorCache = new_Record();

/**
 * Compare this String against that String, using the locale and collation
 * options provided.
 *
 * Spec: ECMAScript Internationalization API Specification, 13.1.1.
 */
function String_localeCompare(that) {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("localeCompare", this);
  }

  // Steps 2-3.
  var S = ToString(this);
  var That = ToString(that);

  // Steps 4-5.
  var locales = ArgumentsLength() > 1 ? GetArgument(1) : undefined;
  var options = ArgumentsLength() > 2 ? GetArgument(2) : undefined;

  // Step 6.
  var collator;
  if (locales === undefined && options === undefined) {
    // This cache only optimizes for the old ES5 localeCompare without
    // locales and options.
    if (!intl_IsRuntimeDefaultLocale(collatorCache.runtimeDefaultLocale)) {
      collatorCache.collator = intl_Collator(locales, options);
      collatorCache.runtimeDefaultLocale = intl_RuntimeDefaultLocale();
    }
    collator = collatorCache.collator;
  } else {
    collator = intl_Collator(locales, options);
  }

  // Step 7.
  return intl_CompareStrings(collator, S, That);
}

/**
 * 13.1.2 String.prototype.toLocaleLowerCase ( [ locales ] )
 *
 * ES2017 Intl draft rev 94045d234762ad107a3d09bb6f7381a65f1a2f9b
 */
function String_toLocaleLowerCase() {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("toLocaleLowerCase", this);
  }

  // Step 2.
  var string = ToString(this);

  // Handle the common cases (no locales argument or a single string
  // argument) first.
  var locales = ArgumentsLength() ? GetArgument(0) : undefined;
  var requestedLocale;
  if (locales === undefined) {
    // Steps 3, 6.
    requestedLocale = undefined;
  } else if (typeof locales === "string") {
    // Steps 3, 5.
    requestedLocale = intl_ValidateAndCanonicalizeLanguageTag(locales, false);
  } else {
    // Step 3.
    var requestedLocales = CanonicalizeLocaleList(locales);

    // Steps 4-6.
    requestedLocale = requestedLocales.length ? requestedLocales[0] : undefined;
  }

  // Trivial case: When the input is empty, directly return the empty string.
  if (string.length === 0) {
    return "";
  }

  if (requestedLocale === undefined) {
    requestedLocale = DefaultLocale();
  }

  // Steps 7-16.
  return intl_toLocaleLowerCase(string, requestedLocale);
}

/**
 * 13.1.3 String.prototype.toLocaleUpperCase ( [ locales ] )
 *
 * ES2017 Intl draft rev 94045d234762ad107a3d09bb6f7381a65f1a2f9b
 */
function String_toLocaleUpperCase() {
  // Step 1.
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("toLocaleUpperCase", this);
  }

  // Step 2.
  var string = ToString(this);

  // Handle the common cases (no locales argument or a single string
  // argument) first.
  var locales = ArgumentsLength() ? GetArgument(0) : undefined;
  var requestedLocale;
  if (locales === undefined) {
    // Steps 3, 6.
    requestedLocale = undefined;
  } else if (typeof locales === "string") {
    // Steps 3, 5.
    requestedLocale = intl_ValidateAndCanonicalizeLanguageTag(locales, false);
  } else {
    // Step 3.
    var requestedLocales = CanonicalizeLocaleList(locales);

    // Steps 4-6.
    requestedLocale = requestedLocales.length ? requestedLocales[0] : undefined;
  }

  // Trivial case: When the input is empty, directly return the empty string.
  if (string.length === 0) {
    return "";
  }

  if (requestedLocale === undefined) {
    requestedLocale = DefaultLocale();
  }

  // Steps 7-16.
  return intl_toLocaleUpperCase(string, requestedLocale);
}
#endif  // JS_HAS_INTL_API

// ES2018 draft rev 8fadde42cf6a9879b4ab0cb6142b31c4ee501667
// 21.1.2.4 String.raw ( template, ...substitutions )
function String_static_raw(callSite /*, ...substitutions*/) {
  // Steps 1-2 (not applicable).

  // Step 3.
  var cooked = ToObject(callSite);

  // Step 4.
  var raw = ToObject(cooked.raw);

  // Step 5.
  var literalSegments = ToLength(raw.length);

  // Step 6.
  if (literalSegments === 0) {
    return "";
  }

  // Special case for |String.raw `<literal>`| callers to avoid falling into
  // the loop code below.
  if (literalSegments === 1) {
    return ToString(raw[0]);
  }

  // Steps 7-9 were reordered to use ArgumentsLength/GetArgument instead of a
  // rest parameter, because the former is currently more optimized.
  //
  // String.raw intersperses the substitution elements between the literal
  // segments, i.e. a substitution is added iff there are still pending
  // literal segments. Furthermore by moving the access to |raw[0]| outside
  // of the loop, we can use |nextIndex| to index into both, the |raw| array
  // and the arguments.

  // Steps 7 (implicit) and 9.a-c.
  var resultString = ToString(raw[0]);

  // Steps 8-9, 9.d, and 9.i.
  for (var nextIndex = 1; nextIndex < literalSegments; nextIndex++) {
    // Steps 9.e-h.
    if (nextIndex < ArgumentsLength()) {
      resultString += ToString(GetArgument(nextIndex));
    }

    // Steps 9.a-c.
    resultString += ToString(raw[nextIndex]);
  }

  // Step 9.d.i.
  return resultString;
}

// ES6 draft 2014-04-27 B.2.3.3
function String_big() {
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("big", this);
  }
  return "<big>" + ToString(this) + "</big>";
}

// ES6 draft 2014-04-27 B.2.3.4
function String_blink() {
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("blink", this);
  }
  return "<blink>" + ToString(this) + "</blink>";
}

// ES6 draft 2014-04-27 B.2.3.5
function String_bold() {
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("bold", this);
  }
  return "<b>" + ToString(this) + "</b>";
}

// ES6 draft 2014-04-27 B.2.3.6
function String_fixed() {
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("fixed", this);
  }
  return "<tt>" + ToString(this) + "</tt>";
}

// ES6 draft 2014-04-27 B.2.3.9
function String_italics() {
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("italics", this);
  }
  return "<i>" + ToString(this) + "</i>";
}

// ES6 draft 2014-04-27 B.2.3.11
function String_small() {
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("small", this);
  }
  return "<small>" + ToString(this) + "</small>";
}

// ES6 draft 2014-04-27 B.2.3.12
function String_strike() {
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("strike", this);
  }
  return "<strike>" + ToString(this) + "</strike>";
}

// ES6 draft 2014-04-27 B.2.3.13
function String_sub() {
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("sub", this);
  }
  return "<sub>" + ToString(this) + "</sub>";
}

// ES6 draft 2014-04-27 B.2.3.14
function String_sup() {
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("sup", this);
  }
  return "<sup>" + ToString(this) + "</sup>";
}

function EscapeAttributeValue(v) {
  var inputStr = ToString(v);
  return StringReplaceAllString(inputStr, '"', "&quot;");
}

// ES6 draft 2014-04-27 B.2.3.2
function String_anchor(name) {
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("anchor", this);
  }
  var S = ToString(this);
  return '<a name="' + EscapeAttributeValue(name) + '">' + S + "</a>";
}

// ES6 draft 2014-04-27 B.2.3.7
function String_fontcolor(color) {
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("fontcolor", this);
  }
  var S = ToString(this);
  return '<font color="' + EscapeAttributeValue(color) + '">' + S + "</font>";
}

// ES6 draft 2014-04-27 B.2.3.8
function String_fontsize(size) {
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("fontsize", this);
  }
  var S = ToString(this);
  return '<font size="' + EscapeAttributeValue(size) + '">' + S + "</font>";
}

// ES6 draft 2014-04-27 B.2.3.10
function String_link(url) {
  if (IsNullOrUndefined(this)) {
    ThrowIncompatibleMethod("link", this);
  }
  var S = ToString(this);
  return '<a href="' + EscapeAttributeValue(url) + '">' + S + "</a>";
}
