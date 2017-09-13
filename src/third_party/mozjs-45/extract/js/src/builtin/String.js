/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*global intl_Collator: false, */

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
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, 'String.substring');
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
    var resultLength = std_Math_min(std_Math_max(end, 0), size - intStart)

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
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, 'String.substr');
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
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, 'String.slice');
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

var collatorCache = new Record();

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

    if (!(n * S.length < (1 << 28)))
        ThrowRangeError(JSMSG_RESULTING_STRING_TOO_LARGE);

    // Communicate |n|'s possible range to the compiler.
    n = n & ((1 << 28) - 1);

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
    if (!IsObject(this) || !IsStringIterator(this)) {
        return callFunction(CallStringIteratorMethodIfWrapped, this,
                            "StringIteratorNext");
    }

    var S = UnsafeGetStringFromReservedSlot(this, ITERATOR_SLOT_TARGET);
    // We know that JSString::MAX_LENGTH <= INT32_MAX (and assert this in
    // SelfHostring.cpp) so our current index can never be anything other than
    // an Int32Value.
    var index = UnsafeGetInt32FromReservedSlot(this, ITERATOR_SLOT_NEXT_INDEX);
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
            charCount = 2;
        }
    }

    UnsafeSetReservedSlot(this, ITERATOR_SLOT_NEXT_INDEX, index + charCount);
    result.value = callFunction(std_String_substring, S, index, index + charCount);

    return result;
}

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
        if (collatorCache.collator === undefined)
            collatorCache.collator = intl_Collator(locales, options);
        collator = collatorCache.collator;
    } else {
        collator = intl_Collator(locales, options);
    }

    // Step 7.
    return intl_CompareStrings(collator, S, That);
}

// ES6 draft rev27 (2014/08/24) 21.1.2.2 String.fromCodePoint(...codePoints)
function String_static_fromCodePoint(codePoints) {
    // Step 1. is not relevant
    // Step 2.
    var length = arguments.length;

    // Step 3.
    var elements = new List();

    // Step 4-5., 5g.
    for (var nextIndex = 0; nextIndex < length; nextIndex++) {
        // Step 5a.
        var next = arguments[nextIndex];
        // Step 5b-c.
        var nextCP = ToNumber(next);

        // Step 5d.
        if (nextCP !== ToInteger(nextCP) || Number_isNaN(nextCP))
            ThrowRangeError(JSMSG_NOT_A_CODEPOINT, ToString(nextCP));

        // Step 5e.
        if (nextCP < 0 || nextCP > 0x10FFFF)
            ThrowRangeError(JSMSG_NOT_A_CODEPOINT, ToString(nextCP));

        // Step 5f.
        // Inlined UTF-16 Encoding
        if (nextCP <= 0xFFFF) {
            callFunction(std_Array_push, elements, nextCP);
            continue;
        }

        callFunction(std_Array_push, elements, (((nextCP - 0x10000) / 0x400) | 0) + 0xD800);
        callFunction(std_Array_push, elements, (nextCP - 0x10000) % 0x400 + 0xDC00);
    }

    // Step 6.
    return callFunction(std_Function_apply, std_String_fromCharCode, null, elements);
}

/* ES6 Draft May 22, 2014 21.1.2.4 */
function String_static_raw(callSite, ...substitutions) {
    // Step 1 (implicit).
    // Step 2.
    var numberOfSubstitutions = substitutions.length;

    // Steps 3-4.
    var cooked = ToObject(callSite);

    // Steps 5-7.
    var raw = ToObject(cooked.raw);

    // Steps 8-10.
    var literalSegments = ToLength(raw.length);

    // Step 11.
    if (literalSegments <= 0)
        return "";

    // Step 12.
    var resultString = "";

    // Step 13.
    var nextIndex = 0;

    // Step 14.
    while (true) {
        // Steps a-d.
        var nextSeg = ToString(raw[nextIndex]);

        // Step e.
        resultString = resultString + nextSeg;

        // Step f.
        if (nextIndex + 1 === literalSegments)
            // Step f.i.
            return resultString;

        // Steps g-j.
        var nextSub;
        if (nextIndex < numberOfSubstitutions)
            nextSub = ToString(substitutions[nextIndex]);
        else
            nextSub = "";

        // Step k.
        resultString = resultString + nextSub;

        // Step l.
        nextIndex++;
    }
}

/**
 * Compare String str1 against String str2, using the locale and collation
 * options provided.
 *
 * Mozilla proprietary.
 * Spec: https://developer.mozilla.org/en-US/docs/JavaScript/Reference/Global_Objects/String#String_generic_methods
 */
function String_static_localeCompare(str1, str2) {
    if (arguments.length < 1)
        ThrowTypeError(JSMSG_MISSING_FUN_ARG, 0, "String.localeCompare");
    var locales = arguments.length > 2 ? arguments[2] : undefined;
    var options = arguments.length > 3 ? arguments[3] : undefined;
    return callFunction(String_localeCompare, str1, str2, locales, options);
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
            outputStr += callFunction(std_String_substring, inputStr, chunkStart, i) + '&quot;';
            chunkStart = i + 1;
        }
    }
    if (chunkStart === 0)
        return inputStr;
    if (chunkStart < inputLen)
        outputStr += callFunction(std_String_substring, inputStr, chunkStart);
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
