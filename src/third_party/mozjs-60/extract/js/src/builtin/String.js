/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*global intl_Collator: false, */

function StringProtoHasNoMatch() {
    var ObjectProto = GetBuiltinPrototype("Object");
    var StringProto = GetBuiltinPrototype("String");
    if (!ObjectHasPrototype(StringProto, ObjectProto))
        return false;
    return !(std_match in StringProto);
}

function IsStringMatchOptimizable() {
    var RegExpProto = GetBuiltinPrototype("RegExp");
    // If RegExpPrototypeOptimizable succeeds, `exec` and `@@match` are
    // guaranteed to be data properties.
    return RegExpPrototypeOptimizable(RegExpProto) &&
           RegExpProto.exec === RegExp_prototype_Exec &&
           RegExpProto[std_match] === RegExpMatch;
}

// ES 2016 draft Mar 25, 2016 21.1.3.11.
function String_match(regexp) {
    // Step 1.
    RequireObjectCoercible(this);

    // Step 2.
    var isPatternString = (typeof regexp === "string");
    if (!(isPatternString && StringProtoHasNoMatch()) && regexp !== undefined && regexp !== null) {
        // Step 2.a.
        var matcher = GetMethod(regexp, std_match);

        // Step 2.b.
        if (matcher !== undefined)
            return callContentFunction(matcher, regexp, this);
    }

    // Step 3.
    var S = ToString(this);

    if (isPatternString && IsStringMatchOptimizable()) {
        var flatResult = FlatStringMatch(S, regexp);
        if (flatResult !== undefined)
            return flatResult;
    }

    // Step 4.
    var rx = RegExpCreate(regexp);

    // Step 5 (optimized case).
    if (IsStringMatchOptimizable())
        return RegExpMatcher(rx, S, 0);

    // Step 5.
    return callContentFunction(GetMethod(rx, std_match), rx, S);
}

function String_generic_match(thisValue, regexp) {
    WarnDeprecatedStringMethod(STRING_GENERICS_MATCH, "match");
    if (thisValue === undefined)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.match");
    return callFunction(String_match, thisValue, regexp);
}

/**
 * A helper function implementing the logic for both String.prototype.padStart
 * and String.prototype.padEnd as described in ES7 Draft March 29, 2016
 */
function String_pad(maxLength, fillString, padEnd) {
    // Steps 1-2.
    RequireObjectCoercible(this);
    let str = ToString(this);

    // Steps 3-4.
    let intMaxLength = ToLength(maxLength);
    let strLen = str.length;

    // Step 5.
    if (intMaxLength <= strLen)
        return str;

    // Steps 6-7.
    let filler = fillString === undefined ? " " : ToString(fillString);

    // Step 8.
    if (filler === "")
        return str;

    // Throw an error if the final string length exceeds the maximum string
    // length. Perform this check early so we can use int32 operations below.
    if (intMaxLength > MAX_STRING_LENGTH)
        ThrowRangeError(JSMSG_RESULTING_STRING_TOO_LARGE);

    // Step 9.
    let fillLen = intMaxLength - strLen;

    // Step 10.
    // Perform an int32 division to ensure String_repeat is not called with a
    // double to avoid repeated bailouts in ToInteger.
    let truncatedStringFiller = callFunction(String_repeat, filler,
                                             (fillLen / filler.length) | 0);

    truncatedStringFiller += Substring(filler, 0, fillLen % filler.length);

    // Step 11.
    if (padEnd === true)
        return str + truncatedStringFiller;
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
    if (!ObjectHasPrototype(StringProto, ObjectProto))
        return false;
    return !(std_replace in StringProto);
}

// A thin wrapper to call SubstringKernel with int32-typed arguments.
// Caller should check the range of |from| and |length|.
function Substring(str, from, length) {
    assert(typeof str === "string", "|str| should be a string");
    assert(from | 0 === from, "coercing |from| into int32 should not change the value");
    assert(length | 0 === length, "coercing |length| into int32 should not change the value");

    return SubstringKernel(str, from | 0, length | 0);
}

// ES 2016 draft Mar 25, 2016 21.1.3.14.
function String_replace(searchValue, replaceValue) {
    // Step 1.
    RequireObjectCoercible(this);

    // Step 2.
    if (!(typeof searchValue === "string" && StringProtoHasNoReplace()) &&
        searchValue !== undefined && searchValue !== null)
    {
        // Step 2.a.
        var replacer = GetMethod(searchValue, std_replace);

        // Step 2.b.
        if (replacer !== undefined)
            return callContentFunction(replacer, searchValue, this, replaceValue);
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
    if (pos === -1)
        return string;

    // Step 8.
    var replStr = ToString(callContentFunction(replaceValue, undefined, searchString, pos, string));

    // Step 10.
    var tailPos = pos + searchString.length;

    // Step 11.
    var newString;
    if (pos === 0)
        newString = "";
    else
        newString = Substring(string, 0, pos);

    newString += replStr;
    var stringLength = string.length;
    if (tailPos < stringLength)
        newString += Substring(string, tailPos, stringLength - tailPos);

    // Step 12.
    return newString;
}

function String_generic_replace(thisValue, searchValue, replaceValue) {
    WarnDeprecatedStringMethod(STRING_GENERICS_REPLACE, "replace");
    if (thisValue === undefined)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.replace");
    return callFunction(String_replace, thisValue, searchValue, replaceValue);
}

function StringProtoHasNoSearch() {
    var ObjectProto = GetBuiltinPrototype("Object");
    var StringProto = GetBuiltinPrototype("String");
    if (!ObjectHasPrototype(StringProto, ObjectProto))
        return false;
    return !(std_search in StringProto);
}

function IsStringSearchOptimizable() {
    var RegExpProto = GetBuiltinPrototype("RegExp");
    // If RegExpPrototypeOptimizable succeeds, `exec` and `@@search` are
    // guaranteed to be data properties.
    return RegExpPrototypeOptimizable(RegExpProto) &&
           RegExpProto.exec === RegExp_prototype_Exec &&
           RegExpProto[std_search] === RegExpSearch;
}

// ES 2016 draft Mar 25, 2016 21.1.3.15.
function String_search(regexp) {
    // Step 1.
    RequireObjectCoercible(this);

    // Step 2.
    var isPatternString = (typeof regexp === "string");
    if (!(isPatternString && StringProtoHasNoSearch()) && regexp !== undefined && regexp !== null) {
        // Step 2.a.
        var searcher = GetMethod(regexp, std_search);

        // Step 2.b.
        if (searcher !== undefined)
            return callContentFunction(searcher, regexp, this);
    }

    // Step 3.
    var string = ToString(this);

    if (isPatternString && IsStringSearchOptimizable()) {
        var flatResult = FlatStringSearch(string, regexp);
        if (flatResult !== -2)
            return flatResult;
    }

    // Step 4.
    var rx = RegExpCreate(regexp);

    // Step 5.
    return callContentFunction(GetMethod(rx, std_search), rx, string);
}

function String_generic_search(thisValue, regexp) {
    WarnDeprecatedStringMethod(STRING_GENERICS_SEARCH, "search");
    if (thisValue === undefined)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.search");
    return callFunction(String_search, thisValue, regexp);
}

function StringProtoHasNoSplit() {
    var ObjectProto = GetBuiltinPrototype("Object");
    var StringProto = GetBuiltinPrototype("String");
    if (!ObjectHasPrototype(StringProto, ObjectProto))
        return false;
    return !(std_split in StringProto);
}

// ES 2016 draft Mar 25, 2016 21.1.3.17.
function String_split(separator, limit) {
    // Step 1.
    RequireObjectCoercible(this);

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
    if (!(typeof separator == "string" && StringProtoHasNoSplit()) &&
        separator !== undefined && separator !== null)
    {
        // Step 2.a.
        var splitter = GetMethod(separator, std_split);

        // Step 2.b.
        if (splitter !== undefined)
            return callContentFunction(splitter, separator, this, limit);
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
        if (lim === 0)
            return [];

        // Step 11.
        if (separator === undefined)
            return [S];

        // Steps 4, 8, 12-18.
        return StringSplitStringLimit(S, R, lim);
    }

    // Step 9.
    R = ToString(separator);

    // Step 11.
    if (separator === undefined)
        return [S];

    // Optimized path.
    // Steps 4, 8, 12-18.
    return StringSplitString(S, R);
}

function String_generic_split(thisValue, separator, limit) {
    WarnDeprecatedStringMethod(STRING_GENERICS_SPLIT, "split");
    if (thisValue === undefined)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.split");
    return callFunction(String_split, thisValue, separator, limit);
}

/* ES6 Draft Oct 14, 2014 21.1.3.19 */
function String_substring(start, end) {
    // Steps 1-3.
    RequireObjectCoercible(this);
    var str = ToString(this);

    // Step 4.
    var len = str.length;

    // Step 5.
    var intStart = ToInteger(start);

    // Step 6.
    var intEnd = (end === undefined) ? len : ToInteger(end);

    // Step 7.
    var finalStart = std_Math_min(std_Math_max(intStart, 0), len);

    // Step 8.
    var finalEnd = std_Math_min(std_Math_max(intEnd, 0), len);

    // Steps 9-10.
    var from, to;
    if (finalStart < finalEnd) {
        from = finalStart;
        to = finalEnd;
    } else {
        from = finalEnd;
        to = finalStart;
    }

    // Step 11.
    // While |from| and |to - from| are bounded to the length of |str| and this
    // and thus definitely in the int32 range, they can still be typed as
    // double. Eagerly truncate since SubstringKernel only accepts int32.
    return SubstringKernel(str, from | 0, (to - from) | 0);
}

function String_static_substring(string, start, end) {
    WarnDeprecatedStringMethod(STRING_GENERICS_SUBSTRING, "substring");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.substring");
    return callFunction(String_substring, string, start, end);
}

/* ES6 Draft Oct 14, 2014 B.2.3.1 */
function String_substr(start, length) {
    // Steps 1-2.
    RequireObjectCoercible(this);
    var str = ToString(this);

    // Steps 3-4.
    var intStart = ToInteger(start);

    // Steps 5-7.
    var size = str.length;
    // Use |size| instead of +Infinity to avoid performing calculations with
    // doubles. (The result is the same either way.)
    var end = (length === undefined) ? size : ToInteger(length);

    // Step 8.
    if (intStart < 0)
        intStart = std_Math_max(intStart + size, 0);

    // Step 9.
    var resultLength = std_Math_min(std_Math_max(end, 0), size - intStart);

    // Step 10.
    if (resultLength <= 0)
        return "";

    // Step 11.
    // While |intStart| and |resultLength| are bounded to the length of |str|
    // and thus definitely in the int32 range, they can still be typed as
    // double. Eagerly truncate since SubstringKernel only accepts int32.
    return SubstringKernel(str, intStart | 0, resultLength | 0);
}

function String_static_substr(string, start, length) {
    WarnDeprecatedStringMethod(STRING_GENERICS_SUBSTR, "substr");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.substr");
    return callFunction(String_substr, string, start, length);
}

/* ES6 Draft Oct 14, 2014 21.1.3.16 */
function String_slice(start, end) {
    // Steps 1-3.
    RequireObjectCoercible(this);
    var str = ToString(this);

    // Step 4.
    var len = str.length;

    // Step 5.
    var intStart = ToInteger(start);

    // Step 6.
    var intEnd = (end === undefined) ? len : ToInteger(end);

    // Step 7.
    var from = (intStart < 0) ? std_Math_max(len + intStart, 0) : std_Math_min(intStart, len);

    // Step 8.
    var to = (intEnd < 0) ? std_Math_max(len + intEnd, 0) : std_Math_min(intEnd, len);

    // Step 9.
    var span = std_Math_max(to - from, 0);

    // Step 10.
    // While |from| and |span| are bounded to the length of |str|
    // and thus definitely in the int32 range, they can still be typed as
    // double. Eagerly truncate since SubstringKernel only accepts int32.
    return SubstringKernel(str, from | 0, span | 0);
}

function String_static_slice(string, start, end) {
    WarnDeprecatedStringMethod(STRING_GENERICS_SLICE, "slice");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.slice");
    return callFunction(String_slice, string, start, end);
}

/* ES6 Draft September 5, 2013 21.1.3.3 */
function String_codePointAt(pos) {
    // Steps 1-3.
    RequireObjectCoercible(this);
    var S = ToString(this);

    // Steps 4-5.
    var position = ToInteger(pos);

    // Step 6.
    var size = S.length;

    // Step 7.
    if (position < 0 || position >= size)
        return undefined;

    // Steps 8-9.
    var first = callFunction(std_String_charCodeAt, S, position);
    if (first < 0xD800 || first > 0xDBFF || position + 1 === size)
        return first;

    // Steps 10-11.
    var second = callFunction(std_String_charCodeAt, S, position + 1);
    if (second < 0xDC00 || second > 0xDFFF)
        return first;

    // Step 12.
    return (first - 0xD800) * 0x400 + (second - 0xDC00) + 0x10000;
}

/* ES6 20121122 draft 15.5.4.21. */
function String_repeat(count) {
    // Steps 1-3.
    RequireObjectCoercible(this);
    var S = ToString(this);

    // Steps 4-5.
    var n = ToInteger(count);

    // Steps 6-7.
    if (n < 0)
        ThrowRangeError(JSMSG_NEGATIVE_REPETITION_COUNT);

    // Inverted condition to handle |Infinity * 0 = NaN| correctly.
    if (!(n * S.length <= MAX_STRING_LENGTH))
        ThrowRangeError(JSMSG_RESULTING_STRING_TOO_LARGE);

    // Communicate |n|'s possible range to the compiler.
    assert((MAX_STRING_LENGTH & (MAX_STRING_LENGTH + 1)) === 0,
           "MAX_STRING_LENGTH can be used as a bitmask");
    n = n & MAX_STRING_LENGTH;

    // Steps 8-9.
    var T = "";
    for (;;) {
        if (n & 1)
            T += S;
        n >>= 1;
        if (n)
            S += S;
        else
            break;
    }
    return T;
}

// ES6 draft specification, section 21.1.3.27, version 2013-09-27.
function String_iterator() {
    RequireObjectCoercible(this);
    var S = ToString(this);
    var iterator = NewStringIterator();
    UnsafeSetReservedSlot(iterator, ITERATOR_SLOT_TARGET, S);
    UnsafeSetReservedSlot(iterator, ITERATOR_SLOT_NEXT_INDEX, 0);
    return iterator;
}

function StringIteratorNext() {
    var obj;
    if (!IsObject(this) || (obj = GuardToStringIterator(this)) === null) {
        return callFunction(CallStringIteratorMethodIfWrapped, this,
                            "StringIteratorNext");
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

    var charCount = 1;
    var first = callFunction(std_String_charCodeAt, S, index);
    if (first >= 0xD800 && first <= 0xDBFF && index + 1 < size) {
        var second = callFunction(std_String_charCodeAt, S, index + 1);
        if (second >= 0xDC00 && second <= 0xDFFF) {
            first = (first - 0xD800) * 0x400 + (second - 0xDC00) + 0x10000;
            charCount = 2;
        }
    }

    UnsafeSetReservedSlot(obj, ITERATOR_SLOT_NEXT_INDEX, index + charCount);

    // Communicate |first|'s possible range to the compiler.
    result.value = callFunction(std_String_fromCodePoint, null, first & 0x1fffff);

    return result;
}

var collatorCache = new Record();

/**
 * Compare this String against that String, using the locale and collation
 * options provided.
 *
 * Spec: ECMAScript Internationalization API Specification, 13.1.1.
 */
function String_localeCompare(that) {
    // Steps 1-3.
    RequireObjectCoercible(this);
    var S = ToString(this);
    var That = ToString(that);

    // Steps 4-5.
    var locales = arguments.length > 1 ? arguments[1] : undefined;
    var options = arguments.length > 2 ? arguments[2] : undefined;

    // Step 6.
    var collator;
    if (locales === undefined && options === undefined) {
        // This cache only optimizes for the old ES5 localeCompare without
        // locales and options.
        if (!IsRuntimeDefaultLocale(collatorCache.runtimeDefaultLocale)) {
            collatorCache.collator = intl_Collator(locales, options);
            collatorCache.runtimeDefaultLocale = RuntimeDefaultLocale();
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
    RequireObjectCoercible(this);

    // Step 2.
    var string = ToString(this);

    // Handle the common cases (no locales argument or a single string
    // argument) first.
    var locales = arguments.length > 0 ? arguments[0] : undefined;
    var requestedLocale;
    if (locales === undefined) {
        // Steps 3, 6.
        requestedLocale = undefined;
    } else if (typeof locales === "string") {
        // Steps 3, 5.
        requestedLocale = ValidateAndCanonicalizeLanguageTag(locales);
    } else {
        // Step 3.
        var requestedLocales = CanonicalizeLocaleList(locales);

        // Steps 4-6.
        requestedLocale = requestedLocales.length > 0 ? requestedLocales[0] : undefined;
    }

    // Trivial case: When the input is empty, directly return the empty string.
    if (string.length === 0)
        return "";

    if (requestedLocale === undefined)
        requestedLocale = DefaultLocale();

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
    RequireObjectCoercible(this);

    // Step 2.
    var string = ToString(this);

    // Handle the common cases (no locales argument or a single string
    // argument) first.
    var locales = arguments.length > 0 ? arguments[0] : undefined;
    var requestedLocale;
    if (locales === undefined) {
        // Steps 3, 6.
        requestedLocale = undefined;
    } else if (typeof locales === "string") {
        // Steps 3, 5.
        requestedLocale = ValidateAndCanonicalizeLanguageTag(locales);
    } else {
        // Step 3.
        var requestedLocales = CanonicalizeLocaleList(locales);

        // Steps 4-6.
        requestedLocale = requestedLocales.length > 0 ? requestedLocales[0] : undefined;
    }

    // Trivial case: When the input is empty, directly return the empty string.
    if (string.length === 0)
        return "";

    if (requestedLocale === undefined)
        requestedLocale = DefaultLocale();

    // Steps 7-16.
    return intl_toLocaleUpperCase(string, requestedLocale);
}

// ES2018 draft rev 8fadde42cf6a9879b4ab0cb6142b31c4ee501667
// 21.1.2.4 String.raw ( template, ...substitutions )
function String_static_raw(callSite/*, ...substitutions*/) {
    // Steps 1-2 (not applicable).

    // Step 3.
    var cooked = ToObject(callSite);

    // Step 4.
    var raw = ToObject(cooked.raw);

    // Step 5.
    var literalSegments = ToLength(raw.length);

    // Step 6.
    if (literalSegments === 0)
        return "";

    // Special case for |String.raw `<literal>`| callers to avoid falling into
    // the loop code below.
    if (literalSegments === 1)
        return ToString(raw[0]);

    // Steps 7-9 were reordered to use the arguments object instead of a rest
    // parameter, because the former is currently more optimized.
    //
    // String.raw intersperses the substitution elements between the literal
    // segments, i.e. a substitution is added iff there are still pending
    // literal segments. Furthermore by moving the access to |raw[0]| outside
    // of the loop, we can use |nextIndex| to index into both, the |raw| array
    // and the arguments object.

    // Steps 7 (implicit) and 9.a-c.
    var resultString = ToString(raw[0]);

    // Steps 8-9, 9.d, and 9.i.
    for (var nextIndex = 1; nextIndex < literalSegments; nextIndex++) {
        // Steps 9.e-h.
        if (nextIndex < arguments.length)
            resultString += ToString(arguments[nextIndex]);

        // Steps 9.a-c.
        resultString += ToString(raw[nextIndex]);
    }

    // Step 9.d.i.
    return resultString;
}

/**
 * Compare String str1 against String str2, using the locale and collation
 * options provided.
 *
 * Mozilla proprietary.
 * Spec: https://developer.mozilla.org/en-US/docs/JavaScript/Reference/Global_Objects/String#String_generic_methods
 */
function String_static_localeCompare(str1, str2) {
    WarnDeprecatedStringMethod(STRING_GENERICS_LOCALE_COMPARE, "localeCompare");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.localeCompare");
    var locales = arguments.length > 2 ? arguments[2] : undefined;
    var options = arguments.length > 3 ? arguments[3] : undefined;
/* eslint-disable no-unreachable */
#if EXPOSE_INTL_API
    return callFunction(String_localeCompare, str1, str2, locales, options);
#else
    return callFunction(std_String_localeCompare, str1, str2, locales, options);
#endif
/* eslint-enable no-unreachable */
}

// ES6 draft 2014-04-27 B.2.3.3
function String_big() {
    RequireObjectCoercible(this);
    return "<big>" + ToString(this) + "</big>";
}

// ES6 draft 2014-04-27 B.2.3.4
function String_blink() {
    RequireObjectCoercible(this);
    return "<blink>" + ToString(this) + "</blink>";
}

// ES6 draft 2014-04-27 B.2.3.5
function String_bold() {
    RequireObjectCoercible(this);
    return "<b>" + ToString(this) + "</b>";
}

// ES6 draft 2014-04-27 B.2.3.6
function String_fixed() {
    RequireObjectCoercible(this);
    return "<tt>" + ToString(this) + "</tt>";
}

// ES6 draft 2014-04-27 B.2.3.9
function String_italics() {
    RequireObjectCoercible(this);
    return "<i>" + ToString(this) + "</i>";
}

// ES6 draft 2014-04-27 B.2.3.11
function String_small() {
    RequireObjectCoercible(this);
    return "<small>" + ToString(this) + "</small>";
}

// ES6 draft 2014-04-27 B.2.3.12
function String_strike() {
    RequireObjectCoercible(this);
    return "<strike>" + ToString(this) + "</strike>";
}

// ES6 draft 2014-04-27 B.2.3.13
function String_sub() {
    RequireObjectCoercible(this);
    return "<sub>" + ToString(this) + "</sub>";
}

// ES6 draft 2014-04-27 B.2.3.14
function String_sup() {
    RequireObjectCoercible(this);
    return "<sup>" + ToString(this) + "</sup>";
}

function EscapeAttributeValue(v) {
    var inputStr = ToString(v);
    var inputLen = inputStr.length;
    var outputStr = "";
    var chunkStart = 0;
    for (var i = 0; i < inputLen; i++) {
        if (inputStr[i] === '"') {
            outputStr += callFunction(String_substring, inputStr, chunkStart, i) + "&quot;";
            chunkStart = i + 1;
        }
    }
    if (chunkStart === 0)
        return inputStr;
    if (chunkStart < inputLen)
        outputStr += callFunction(String_substring, inputStr, chunkStart);
    return outputStr;
}

// ES6 draft 2014-04-27 B.2.3.2
function String_anchor(name) {
    RequireObjectCoercible(this);
    var S = ToString(this);
    return '<a name="' + EscapeAttributeValue(name) + '">' + S + "</a>";
}

// ES6 draft 2014-04-27 B.2.3.7
function String_fontcolor(color) {
    RequireObjectCoercible(this);
    var S = ToString(this);
    return '<font color="' + EscapeAttributeValue(color) + '">' + S + "</font>";
}

// ES6 draft 2014-04-27 B.2.3.8
function String_fontsize(size) {
    RequireObjectCoercible(this);
    var S = ToString(this);
    return '<font size="' + EscapeAttributeValue(size) + '">' + S + "</font>";
}

// ES6 draft 2014-04-27 B.2.3.10
function String_link(url) {
    RequireObjectCoercible(this);
    var S = ToString(this);
    return '<a href="' + EscapeAttributeValue(url) + '">' + S + "</a>";
}

function String_static_toLowerCase(string) {
    WarnDeprecatedStringMethod(STRING_GENERICS_TO_LOWER_CASE, "toLowerCase");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.toLowerCase");
    return callFunction(std_String_toLowerCase, string);
}

function String_static_toUpperCase(string) {
    WarnDeprecatedStringMethod(STRING_GENERICS_TO_UPPER_CASE, "toUpperCase");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.toUpperCase");
    return callFunction(std_String_toUpperCase, string);
}

function String_static_charAt(string, pos) {
    WarnDeprecatedStringMethod(STRING_GENERICS_CHAR_AT, "charAt");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.charAt");
    return callFunction(std_String_charAt, string, pos);
}

function String_static_charCodeAt(string, pos) {
    WarnDeprecatedStringMethod(STRING_GENERICS_CHAR_CODE_AT, "charCodeAt");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.charCodeAt");
    return callFunction(std_String_charCodeAt, string, pos);
}

function String_static_includes(string, searchString) {
    WarnDeprecatedStringMethod(STRING_GENERICS_INCLUDES, "includes");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.includes");
    var position = arguments.length > 2 ? arguments[2] : undefined;
    return callFunction(std_String_includes, string, searchString, position);
}

function String_static_indexOf(string, searchString) {
    WarnDeprecatedStringMethod(STRING_GENERICS_INDEX_OF, "indexOf");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.indexOf");
    var position = arguments.length > 2 ? arguments[2] : undefined;
    return callFunction(std_String_indexOf, string, searchString, position);
}

function String_static_lastIndexOf(string, searchString) {
    WarnDeprecatedStringMethod(STRING_GENERICS_LAST_INDEX_OF, "lastIndexOf");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.lastIndexOf");
    var position = arguments.length > 2 ? arguments[2] : undefined;
    return callFunction(std_String_lastIndexOf, string, searchString, position);
}

function String_static_startsWith(string, searchString) {
    WarnDeprecatedStringMethod(STRING_GENERICS_STARTS_WITH, "startsWith");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.startsWith");
    var position = arguments.length > 2 ? arguments[2] : undefined;
    return callFunction(std_String_startsWith, string, searchString, position);
}

function String_static_endsWith(string, searchString) {
    WarnDeprecatedStringMethod(STRING_GENERICS_ENDS_WITH, "endsWith");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.endsWith");
    var endPosition = arguments.length > 2 ? arguments[2] : undefined;
    return callFunction(std_String_endsWith, string, searchString, endPosition);
}

function String_static_trim(string) {
    WarnDeprecatedStringMethod(STRING_GENERICS_TRIM, "trim");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.trim");
    return callFunction(std_String_trim, string);
}

function String_static_trimLeft(string) {
    WarnDeprecatedStringMethod(STRING_GENERICS_TRIM_LEFT, "trimLeft");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.trimLeft");
    return callFunction(std_String_trimStart, string);
}

function String_static_trimRight(string) {
    WarnDeprecatedStringMethod(STRING_GENERICS_TRIM_RIGHT, "trimRight");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.trimRight");
    return callFunction(std_String_trimEnd, string);
}

function String_static_toLocaleLowerCase(string) {
    WarnDeprecatedStringMethod(STRING_GENERICS_TO_LOCALE_LOWER_CASE, "toLocaleLowerCase");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.toLocaleLowerCase");
/* eslint-disable no-unreachable */
#if EXPOSE_INTL_API
    var locales = arguments.length > 1 ? arguments[1] : undefined;
    return callFunction(String_toLocaleLowerCase, string, locales);
#else
    return callFunction(std_String_toLocaleLowerCase, string);
#endif
/* eslint-enable no-unreachable */
}

function String_static_toLocaleUpperCase(string) {
    WarnDeprecatedStringMethod(STRING_GENERICS_TO_LOCALE_UPPER_CASE, "toLocaleUpperCase");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.toLocaleUpperCase");
/* eslint-disable no-unreachable */
#if EXPOSE_INTL_API
    var locales = arguments.length > 1 ? arguments[1] : undefined;
    return callFunction(String_toLocaleUpperCase, string, locales);
#else
    return callFunction(std_String_toLocaleUpperCase, string);
#endif
/* eslint-enable no-unreachable */
}

#if EXPOSE_INTL_API
function String_static_normalize(string) {
    WarnDeprecatedStringMethod(STRING_GENERICS_NORMALIZE, "normalize");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.normalize");
    var form = arguments.length > 1 ? arguments[1] : undefined;
    return callFunction(std_String_normalize, string, form);
}
#endif

function String_static_concat(string, arg1) {
    WarnDeprecatedStringMethod(STRING_GENERICS_CONCAT, "concat");
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.concat");
    var args = callFunction(std_Array_slice, arguments, 1);
    return callFunction(std_Function_apply, std_String_concat, string, args);
}
