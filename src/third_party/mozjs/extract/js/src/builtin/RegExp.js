/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ES6 draft rev34 (2015/02/20) 21.2.5.3 get RegExp.prototype.flags
function RegExpFlagsGetter() {
    // Steps 1-2.
    var R = this;
    if (!IsObject(R))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, R === null ? "null" : typeof R);

    // Step 3.
    var result = "";

    // Steps 4-6.
    if (R.global)
        result += "g";

    // Steps 7-9.
    if (R.ignoreCase)
        result += "i";

    // Steps 10-12.
    if (R.multiline)
        result += "m";

    // Steps 13-15.
    if (R.unicode)
         result += "u";

    // Steps 16-18.
    if (R.sticky)
        result += "y";

    // Step 19.
    return result;
}
_SetCanonicalName(RegExpFlagsGetter, "get flags");

// ES 2017 draft 40edb3a95a475c1b251141ac681b8793129d9a6d 21.2.5.14.
function RegExpToString()
{
    // Step 1.
    var R = this;

    // Step 2.
    if (!IsObject(R))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, R === null ? "null" : typeof R);

    // Step 3.
    var pattern = ToString(R.source);

    // Step 4.
    var flags = ToString(R.flags);

    // Steps 5-6.
    return "/" + pattern + "/" + flags;
}
_SetCanonicalName(RegExpToString, "toString");

// ES 2016 draft Mar 25, 2016 21.2.5.2.3.
function AdvanceStringIndex(S, index) {
    // Step 1.
    assert(typeof S === "string", "Expected string as 1st argument");

    // Step 2.
    assert(index >= 0 && index <= MAX_NUMERIC_INDEX, "Expected integer as 2nd argument");

    // Step 3 (skipped).

    // Step 4 (skipped).

    // Step 5.
    var length = S.length;

    // Step 6.
    if (index + 1 >= length)
        return index + 1;

    // Step 7.
    var first = callFunction(std_String_charCodeAt, S, index);

    // Step 8.
    if (first < 0xD800 || first > 0xDBFF)
        return index + 1;

    // Step 9.
    var second = callFunction(std_String_charCodeAt, S, index + 1);

    // Step 10.
    if (second < 0xDC00 || second > 0xDFFF)
        return index + 1;

    // Step 11.
    return index + 2;
}

// ES 2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e 21.2.5.6.
function RegExpMatch(string) {
    // Step 1.
    var rx = this;

    // Step 2.
    if (!IsObject(rx))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, rx === null ? "null" : typeof rx);

    // Step 3.
    var S = ToString(string);

    // Optimized paths for simple cases.
    if (IsRegExpMethodOptimizable(rx)) {
        // Step 4.
        var flags = UnsafeGetInt32FromReservedSlot(rx, REGEXP_FLAGS_SLOT);
        var global = !!(flags & REGEXP_GLOBAL_FLAG);

        if (global) {
            // Step 6.a.
            var fullUnicode = !!(flags & REGEXP_UNICODE_FLAG);

            // Steps 6.b-e.
            return RegExpGlobalMatchOpt(rx, S, fullUnicode);
        }

        // Step 5.
        return RegExpBuiltinExec(rx, S, false);
    }

    // Stes 4-6
    return RegExpMatchSlowPath(rx, S);
}

// ES 2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e 21.2.5.6
// steps 4-6.
function RegExpMatchSlowPath(rx, S) {
    // Steps 4-5.
    if (!rx.global)
        return RegExpExec(rx, S, false);

    // Step 6.a.
    var fullUnicode = !!rx.unicode;

    // Step 6.b.
    rx.lastIndex = 0;

    // Step 6.c.
    var A = [];

    // Step 6.d.
    var n = 0;

    // Step 6.e.
    while (true) {
        // Step 6.e.i.
        var result = RegExpExec(rx, S, false);

        // Step 6.e.ii.
        if (result === null)
          return (n === 0) ? null : A;

        // Step 6.e.iii.1.
        var matchStr = ToString(result[0]);

        // Step 6.e.iii.2.
        _DefineDataProperty(A, n, matchStr);

        // Step 6.e.iii.4.
        if (matchStr === "") {
            var lastIndex = ToLength(rx.lastIndex);
            rx.lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
        }

        // Step 6.e.iii.5.
        n++;
    }
}

// ES 2017 draft rev 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e 21.2.5.6.
// Steps 6.b-e.
// Optimized path for @@match with global flag.
function RegExpGlobalMatchOpt(rx, S, fullUnicode) {
    // Step 6.b.
    var lastIndex = 0;
    rx.lastIndex = 0;

    // Step 6.c.
    var A = [];

    // Step 6.d.
    var n = 0;

    var lengthS = S.length;

    // Step 6.e.
    while (true) {
        // Step 6.e.i.
        var result = RegExpMatcher(rx, S, lastIndex);

        // Step 6.e.ii.
        if (result === null)
            return (n === 0) ? null : A;

        lastIndex = result.index + result[0].length;

        // Step 6.e.iii.1.
        var matchStr = result[0];

        // Step 6.e.iii.2.
        _DefineDataProperty(A, n, matchStr);

        // Step 6.e.iii.4.
        if (matchStr === "") {
            lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
            if (lastIndex > lengthS)
                return A;
        }

        // Step 6.e.iii.5.
        n++;
    }
}

// Checks if following properties and getters are not modified, and accessing
// them not observed by content script:
//   * flags
//   * global
//   * ignoreCase
//   * multiline
//   * sticky
//   * unicode
//   * exec
//   * lastIndex
function IsRegExpMethodOptimizable(rx) {
    if (!IsRegExpObject(rx))
        return false;

    var RegExpProto = GetBuiltinPrototype("RegExp");
    // If RegExpPrototypeOptimizable and RegExpInstanceOptimizable succeed,
    // `RegExpProto.exec` is guaranteed to be data properties.
    return RegExpPrototypeOptimizable(RegExpProto) &&
           RegExpInstanceOptimizable(rx, RegExpProto) &&
           RegExpProto.exec === RegExp_prototype_Exec;
}

// ES 2017 draft rev 03bfda119d060aca4099d2b77cf43f6d4f11cfa2 21.2.5.8.
function RegExpReplace(string, replaceValue) {
    // Step 1.
    var rx = this;

    // Step 2.
    if (!IsObject(rx))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, rx === null ? "null" : typeof rx);

    // Step 3.
    var S = ToString(string);

    // Step 4.
    var lengthS = S.length;

    // Step 5.
    var functionalReplace = IsCallable(replaceValue);

    // Step 6.
    var firstDollarIndex = -1;
    if (!functionalReplace) {
        // Step 6.a.
        replaceValue = ToString(replaceValue);

        // Skip if replaceValue is an empty string or a single character.
        // A single character string may contain "$", but that cannot be a
        // substitution.
        if (replaceValue.length > 1)
            firstDollarIndex = GetFirstDollarIndex(replaceValue);
    }

    // Optimized paths.
    if (IsRegExpMethodOptimizable(rx)) {
        var flags = UnsafeGetInt32FromReservedSlot(rx, REGEXP_FLAGS_SLOT);

        // Step 7.
        var global = !!(flags & REGEXP_GLOBAL_FLAG);

        // Steps 8-16.
        if (global) {
            if (functionalReplace) {
                // For large strings check if the replacer function is
                // applicable for the elem-base optimization.
                if (lengthS > 5000) {
                    var elemBase = GetElemBaseForLambda(replaceValue);
                    if (IsObject(elemBase)) {
                        return RegExpGlobalReplaceOptElemBase(rx, S, lengthS, replaceValue, flags,
                                                              elemBase);
                    }
                }
                return RegExpGlobalReplaceOptFunc(rx, S, lengthS, replaceValue, flags);
            }
            if (firstDollarIndex !== -1) {
                return RegExpGlobalReplaceOptSubst(rx, S, lengthS, replaceValue, flags,
                                                   firstDollarIndex);
            }
            if (lengthS < 0x7fff)
                return RegExpGlobalReplaceShortOpt(rx, S, lengthS, replaceValue, flags);
            return RegExpGlobalReplaceOpt(rx, S, lengthS, replaceValue, flags);
        }

        if (functionalReplace)
            return RegExpLocalReplaceOptFunc(rx, S, lengthS, replaceValue);
        if (firstDollarIndex !== -1)
            return RegExpLocalReplaceOptSubst(rx, S, lengthS, replaceValue, firstDollarIndex);
        if (lengthS < 0x7fff)
            return RegExpLocalReplaceOptShort(rx, S, lengthS, replaceValue);
        return RegExpLocalReplaceOpt(rx, S, lengthS, replaceValue);
    }

    // Steps 8-16.
    return RegExpReplaceSlowPath(rx, S, lengthS, replaceValue,
                                 functionalReplace, firstDollarIndex);
}

// ES 2017 draft rev 03bfda119d060aca4099d2b77cf43f6d4f11cfa2 21.2.5.8
// steps 7-16.
// Slow path for @@replace.
function RegExpReplaceSlowPath(rx, S, lengthS, replaceValue,
                               functionalReplace, firstDollarIndex)
{
    // Step 7.
    var global = !!rx.global;

    // Step 8.
    var fullUnicode = false;
    if (global) {
        // Step 8.a.
        fullUnicode = !!rx.unicode;

        // Step 8.b.
        rx.lastIndex = 0;
    }

    // Step 9.
    var results = [];
    var nResults = 0;

    // Step 11.
    while (true) {
        // Step 11.a.
        var result = RegExpExec(rx, S, false);

        // Step 11.b.
        if (result === null)
            break;

        // Step 11.c.i.
        _DefineDataProperty(results, nResults++, result);

        // Step 11.c.ii.
        if (!global)
            break;

        // Step 11.c.iii.1.
        var matchStr = ToString(result[0]);

        // Step 11.c.iii.2.
        if (matchStr === "") {
            var lastIndex = ToLength(rx.lastIndex);
            rx.lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
        }
    }

    // Step 12.
    var accumulatedResult = "";

    // Step 13.
    var nextSourcePosition = 0;

    // Step 14.
    for (var i = 0; i < nResults; i++) {
        result = results[i];

        // Steps 14.a-b.
        var nCaptures = std_Math_max(ToLength(result.length) - 1, 0);

        // Step 14.c.
        var matched = ToString(result[0]);

        // Step 14.d.
        var matchLength = matched.length;

        // Steps 14.e-f.
        var position = std_Math_max(std_Math_min(ToInteger(result.index), lengthS), 0);

        var n, capN, replacement;
        if (functionalReplace || firstDollarIndex !== -1) {
            // Steps 14.g-j.
            replacement = RegExpGetComplexReplacement(result, matched, S, position,
                                                      nCaptures, replaceValue,
                                                      functionalReplace, firstDollarIndex);
        } else {
            // Step 14.g, 14.i, 14.i.iv.
            // We don't need captures array, but ToString is visible to script.
            for (n = 1; n <= nCaptures; n++) {
                // Step 14.i.i-ii.
                capN = result[n];

                // Step 14.i.ii.
                if (capN !== undefined)
                    ToString(capN);
            }
            replacement = replaceValue;
        }

        // Step 14.l.
        if (position >= nextSourcePosition) {
            // Step 14.l.ii.
            accumulatedResult += Substring(S, nextSourcePosition,
                                           position - nextSourcePosition) + replacement;

            // Step 14.l.iii.
            nextSourcePosition = position + matchLength;
        }
    }

    // Step 15.
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;

    // Step 16.
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}

// ES 2017 draft rev 03bfda119d060aca4099d2b77cf43f6d4f11cfa2 21.2.5.8
// steps 14.g-k.
// Calculates functional/substitution replacement from match result.
// Used in the following functions:
//   * RegExpReplaceSlowPath
function RegExpGetComplexReplacement(result, matched, S, position,
                                     nCaptures, replaceValue,
                                     functionalReplace, firstDollarIndex)
{
    // Step 14.h.
    var captures = [];
    var capturesLength = 0;

    // Step 14.j.i (reordered).
    _DefineDataProperty(captures, capturesLength++, matched);

    // Step 14.g, 14.i, 14.i.iv.
    for (var n = 1; n <= nCaptures; n++) {
        // Step 14.i.i.
        var capN = result[n];

        // Step 14.i.ii.
        if (capN !== undefined)
            capN = ToString(capN);

        // Step 14.i.iii.
        _DefineDataProperty(captures, capturesLength++, capN);
    }

    // Step 14.j.
    if (functionalReplace) {
        // For `nCaptures` <= 4 case, call `replaceValue` directly, otherwise
        // use `std_Function_apply` with all arguments stored in `captures`.
        switch (nCaptures) {
          case 0:
            return ToString(replaceValue(SPREAD(captures, 1), position, S));
          case 1:
            return ToString(replaceValue(SPREAD(captures, 2), position, S));
          case 2:
            return ToString(replaceValue(SPREAD(captures, 3), position, S));
          case 3:
            return ToString(replaceValue(SPREAD(captures, 4), position, S));
          case 4:
            return ToString(replaceValue(SPREAD(captures, 5), position, S));
          default:
            // Steps 14.j.ii-v.
            _DefineDataProperty(captures, capturesLength++, position);
            _DefineDataProperty(captures, capturesLength++, S);
            return ToString(callFunction(std_Function_apply, replaceValue, undefined, captures));
        }
    }

    // Steps 14.k.i.
    return RegExpGetSubstitution(captures, S, position, replaceValue, firstDollarIndex);
}

// ES 2017 draft rev 03bfda119d060aca4099d2b77cf43f6d4f11cfa2 21.2.5.8
// steps 14.g-j.
// Calculates functional replacement from match result.
// Used in the following functions:
//   * RegExpGlobalReplaceOptFunc
//   * RegExpGlobalReplaceOptElemBase
//   * RegExpLocalReplaceOptFunc
function RegExpGetFunctionalReplacement(result, S, position, replaceValue) {
    // For `nCaptures` <= 4 case, call `replaceValue` directly, otherwise
    // use `std_Function_apply` with all arguments stored in `captures`.
    assert(result.length >= 1, "RegExpMatcher doesn't return an empty array");
    var nCaptures = result.length - 1;

    switch (nCaptures) {
      case 0:
        return ToString(replaceValue(SPREAD(result, 1), position, S));
      case 1:
        return ToString(replaceValue(SPREAD(result, 2), position, S));
      case 2:
        return ToString(replaceValue(SPREAD(result, 3), position, S));
      case 3:
        return ToString(replaceValue(SPREAD(result, 4), position, S));
      case 4:
        return ToString(replaceValue(SPREAD(result, 5), position, S));
    }

    // Steps 14.g-i, 14.j.i-ii.
    var captures = [];
    for (var n = 0; n <= nCaptures; n++) {
        assert(typeof result[n] === "string" || result[n] === undefined,
               "RegExpMatcher returns only strings and undefined");
        _DefineDataProperty(captures, n, result[n]);
    }

    // Step 14.j.iii.
    _DefineDataProperty(captures, nCaptures + 1, position);
    _DefineDataProperty(captures, nCaptures + 2, S);

    // Steps 14.j.iv-v.
    return ToString(callFunction(std_Function_apply, replaceValue, undefined, captures));
}

// ES 2017 draft rev 03bfda119d060aca4099d2b77cf43f6d4f11cfa2 21.2.5.8
// steps 8.b-16.
// Optimized path for @@replace with the following conditions:
//   * global flag is true
//   * S is a short string (lengthS < 0x7fff)
//   * replaceValue is a string without "$"
function RegExpGlobalReplaceShortOpt(rx, S, lengthS, replaceValue, flags)
{
    // Step 8.a.
    var fullUnicode = !!(flags & REGEXP_UNICODE_FLAG);

    // Step 8.b.
    var lastIndex = 0;
    rx.lastIndex = 0;

    // Step 12 (reordered).
    var accumulatedResult = "";

    // Step 13 (reordered).
    var nextSourcePosition = 0;

    // Step 11.
    while (true) {
        // Step 11.a.
        var result = RegExpSearcher(rx, S, lastIndex);

        // Step 11.b.
        if (result === -1)
            break;

        var position = result & 0x7fff;
        lastIndex = (result >> 15) & 0x7fff;

        // Step 14.l.ii.
        accumulatedResult += Substring(S, nextSourcePosition,
                                       position - nextSourcePosition) + replaceValue;

        // Step 14.l.iii.
        nextSourcePosition = lastIndex;

        // Step 11.c.iii.2.
        if (lastIndex === position) {
            lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
            if (lastIndex > lengthS)
                break;
        }
    }

    // Step 15.
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;

    // Step 16.
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}

// ES 2017 draft rev 03bfda119d060aca4099d2b77cf43f6d4f11cfa2 21.2.5.8
// steps 8-16.
// Optimized path for @@replace.

// Conditions:
//   * global flag is true
//   * replaceValue is a string without "$"
#define FUNC_NAME RegExpGlobalReplaceOpt
#include "RegExpGlobalReplaceOpt.h.js"
#undef FUNC_NAME

// Conditions:
//   * global flag is true
//   * replaceValue is a function
#define FUNC_NAME RegExpGlobalReplaceOptFunc
#define FUNCTIONAL
#include "RegExpGlobalReplaceOpt.h.js"
#undef FUNCTIONAL
#undef FUNC_NAME

// Conditions:
//   * global flag is true
//   * replaceValue is a function that returns element of an object
#define FUNC_NAME RegExpGlobalReplaceOptElemBase
#define ELEMBASE
#include "RegExpGlobalReplaceOpt.h.js"
#undef ELEMBASE
#undef FUNC_NAME

// Conditions:
//   * global flag is true
//   * replaceValue is a string with "$"
#define FUNC_NAME RegExpGlobalReplaceOptSubst
#define SUBSTITUTION
#include "RegExpGlobalReplaceOpt.h.js"
#undef SUBSTITUTION
#undef FUNC_NAME

// Conditions:
//   * global flag is false
//   * replaceValue is a string without "$"
#define FUNC_NAME RegExpLocalReplaceOpt
#include "RegExpLocalReplaceOpt.h.js"
#undef FUNC_NAME

// Conditions:
//   * global flag is false
//   * S is a short string (lengthS < 0x7fff)
//   * replaceValue is a string without "$"
#define FUNC_NAME RegExpLocalReplaceOptShort
#define SHORT_STRING
#include "RegExpLocalReplaceOpt.h.js"
#undef SHORT_STRING
#undef FUNC_NAME

// Conditions:
//   * global flag is false
//   * replaceValue is a function
#define FUNC_NAME RegExpLocalReplaceOptFunc
#define FUNCTIONAL
#include "RegExpLocalReplaceOpt.h.js"
#undef FUNCTIONAL
#undef FUNC_NAME

// Conditions:
//   * global flag is false
//   * replaceValue is a string with "$"
#define FUNC_NAME RegExpLocalReplaceOptSubst
#define SUBSTITUTION
#include "RegExpLocalReplaceOpt.h.js"
#undef SUBSTITUTION
#undef FUNC_NAME

// ES2017 draft rev 6390c2f1b34b309895d31d8c0512eac8660a0210
// 21.2.5.9 RegExp.prototype [ @@search ] ( string )
function RegExpSearch(string) {
    // Step 1.
    var rx = this;

    // Step 2.
    if (!IsObject(rx))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, rx === null ? "null" : typeof rx);

    // Step 3.
    var S = ToString(string);

    // Step 4.
    var previousLastIndex = rx.lastIndex;

    // Step 5.
    var lastIndexIsZero = SameValue(previousLastIndex, 0);
    if (!lastIndexIsZero)
        rx.lastIndex = 0;

    if (IsRegExpMethodOptimizable(rx) && S.length < 0x7fff) {
        // Step 6.
        var result = RegExpSearcher(rx, S, 0);

        // We need to consider two cases:
        //
        // 1. Neither global nor sticky is set:
        // RegExpBuiltinExec doesn't modify lastIndex for local RegExps, that
        // means |SameValue(rx.lastIndex, 0)| is true after calling exec. The
        // comparison in steps 7-8 |SameValue(rx.lastIndex, previousLastIndex)|
        // is therefore equal to the already computed |lastIndexIsZero| value.
        //
        // 2. Global or sticky flag is set.
        // RegExpBuiltinExec will always update lastIndex and we need to
        // restore the property to its original value.

        // Steps 7-8.
        if (!lastIndexIsZero) {
            rx.lastIndex = previousLastIndex;
        } else {
            var flags = UnsafeGetInt32FromReservedSlot(rx, REGEXP_FLAGS_SLOT);
            if (flags & (REGEXP_GLOBAL_FLAG | REGEXP_STICKY_FLAG))
                rx.lastIndex = previousLastIndex;
        }

        // Step 9.
        if (result === -1)
            return -1;

        // Step 10.
        return result & 0x7fff;
    }

    return RegExpSearchSlowPath(rx, S, previousLastIndex);
}

// ES2017 draft rev 6390c2f1b34b309895d31d8c0512eac8660a0210
// 21.2.5.9 RegExp.prototype [ @@search ] ( string )
// Steps 6-10.
function RegExpSearchSlowPath(rx, S, previousLastIndex) {
    // Step 6.
    var result = RegExpExec(rx, S, false);

    // Step 7.
    var currentLastIndex = rx.lastIndex;

    // Step 8.
    if (!SameValue(currentLastIndex, previousLastIndex))
        rx.lastIndex = previousLastIndex;

    // Step 9.
    if (result === null)
        return -1;

    // Step 10.
    return result.index;
}

function IsRegExpSplitOptimizable(rx, C) {
    if (!IsRegExpObject(rx))
        return false;

    var RegExpCtor = GetBuiltinConstructor("RegExp");
    if (C !== RegExpCtor)
        return false;

    var RegExpProto = RegExpCtor.prototype;
    // If RegExpPrototypeOptimizable succeeds, `RegExpProto.exec` is guaranteed
    // to be a data property.
    return RegExpPrototypeOptimizable(RegExpProto) &&
           RegExpInstanceOptimizable(rx, RegExpProto) &&
           RegExpProto.exec === RegExp_prototype_Exec;
}

// ES 2017 draft 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e 21.2.5.11.
function RegExpSplit(string, limit) {
    // Step 1.
    var rx = this;

    // Step 2.
    if (!IsObject(rx))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, rx === null ? "null" : typeof rx);

    // Step 3.
    var S = ToString(string);

    // Step 4.
    var C = SpeciesConstructor(rx, GetBuiltinConstructor("RegExp"));

    var optimizable = IsRegExpSplitOptimizable(rx, C) &&
                      (limit === undefined || typeof limit == "number");

    var flags, unicodeMatching, splitter;
    if (optimizable) {
        // Step 5.
        flags = UnsafeGetInt32FromReservedSlot(rx, REGEXP_FLAGS_SLOT);

        // Steps 6-7.
        unicodeMatching = !!(flags & (REGEXP_UNICODE_FLAG));

        // Steps 8-10.
        // If split operation is optimizable, perform non-sticky match.
        if (flags & REGEXP_STICKY_FLAG) {
            var source = UnsafeGetStringFromReservedSlot(rx, REGEXP_SOURCE_SLOT);
            splitter = regexp_construct_raw_flags(source, flags & ~REGEXP_STICKY_FLAG);
        } else {
            splitter = rx;
        }
    } else {
        // Step 5.
        flags = ToString(rx.flags);

        // Steps 6-7.
        unicodeMatching = callFunction(std_String_includes, flags, "u");

        // Steps 8-9.
        var newFlags;
        if (callFunction(std_String_includes, flags, "y"))
            newFlags = flags;
        else
            newFlags = flags + "y";

        // Step 10.
        splitter = new C(rx, newFlags);
    }

    // Step 11.
    var A = [];

    // Step 12.
    var lengthA = 0;

    // Step 13.
    var lim;
    if (limit === undefined)
        lim = MAX_UINT32;
    else
        lim = limit >>> 0;

    // Step 15.
    var p = 0;

    // Step 16.
    if (lim === 0)
        return A;

    // Step 14 (reordered).
    var size = S.length;

    // Step 17.
    if (size === 0) {
        // Step 17.a.
        var z;
        if (optimizable)
            z = RegExpMatcher(splitter, S, 0);
        else
            z = RegExpExec(splitter, S, false);

        // Step 17.b.
        if (z !== null)
            return A;

        // Step 17.d.
        _DefineDataProperty(A, 0, S);

        // Step 17.e.
        return A;
    }

    // Step 18.
    var q = p;

    // Step 19.
    while (q < size) {
        var e;
        if (optimizable) {
            // Step 19.a (skipped).
            // splitter.lastIndex is not used.

            // Step 19.b.
            z = RegExpMatcher(splitter, S, q);

            // Step 19.c.
            if (z === null)
                break;

            // splitter.lastIndex is not updated.
            q = z.index;
            if (q >= size)
                break;

            // Step 19.d.i.
            e = q + z[0].length;
        } else {
            // Step 19.a.
            splitter.lastIndex = q;

            // Step 19.b.
            z = RegExpExec(splitter, S, false);

            // Step 19.c.
            if (z === null) {
                q = unicodeMatching ? AdvanceStringIndex(S, q) : q + 1;
                continue;
            }

            // Step 19.d.i.
            e = ToLength(splitter.lastIndex);
        }

        // Step 19.d.iii.
        if (e === p) {
            q = unicodeMatching ? AdvanceStringIndex(S, q) : q + 1;
            continue;
        }

        // Steps 19.d.iv.1-3.
        _DefineDataProperty(A, lengthA, Substring(S, p, q - p));

        // Step 19.d.iv.4.
        lengthA++;

        // Step 19.d.iv.5.
        if (lengthA === lim)
            return A;

        // Step 19.d.iv.6.
        p = e;

        // Steps 19.d.iv.7-8.
        var numberOfCaptures = std_Math_max(ToLength(z.length) - 1, 0);

        // Step 19.d.iv.9.
        var i = 1;

        // Step 19.d.iv.10.
        while (i <= numberOfCaptures) {
            // Steps 19.d.iv.10.a-b.
            _DefineDataProperty(A, lengthA, z[i]);

            // Step 19.d.iv.10.c.
            i++;

            // Step 19.d.iv.10.d.
            lengthA++;

            // Step 19.d.iv.10.e.
            if (lengthA === lim)
                return A;
        }

        // Step 19.d.iv.11.
        q = p;
    }

    // Steps 20-22.
    if (p >= size)
        _DefineDataProperty(A, lengthA, "");
    else
        _DefineDataProperty(A, lengthA, Substring(S, p, size - p));

    // Step 23.
    return A;
}

// ES6 21.2.5.2.
// NOTE: This is not RegExpExec (21.2.5.2.1).
function RegExp_prototype_Exec(string) {
    // Steps 1-3.
    var R = this;
    if (!IsObject(R) || !IsRegExpObject(R))
        return callFunction(CallRegExpMethodIfWrapped, R, string, "RegExp_prototype_Exec");

    // Steps 4-5.
    var S = ToString(string);

    // Step 6.
    return RegExpBuiltinExec(R, S, false);
}

// ES6 21.2.5.2.1.
function RegExpExec(R, S, forTest) {
    // Steps 1-2 (skipped).

    // Steps 3-4.
    var exec = R.exec;

    // Step 5.
    // If exec is the original RegExp.prototype.exec, use the same, faster,
    // path as for the case where exec isn't callable.
    if (exec === RegExp_prototype_Exec || !IsCallable(exec)) {
        // ES6 21.2.5.2 steps 1-2, 4-5 (skipped) for optimized case.

        // Steps 6-7 or ES6 21.2.5.2 steps 3, 6 for optimized case.
        return RegExpBuiltinExec(R, S, forTest);
    }

    // Steps 5.a-b.
    var result = callContentFunction(exec, R, S);

    // Step 5.c.
    if (typeof result !== "object")
        ThrowTypeError(JSMSG_EXEC_NOT_OBJORNULL);

    // Step 5.d.
    return forTest ? result !== null : result;
}

// ES2017 draft rev 6390c2f1b34b309895d31d8c0512eac8660a0210
// 21.2.5.2.2 Runtime Semantics: RegExpBuiltinExec ( R, S )
function RegExpBuiltinExec(R, S, forTest) {
    // 21.2.5.2.1 Runtime Semantics: RegExpExec, step 5.
    // This check is here for RegExpTest.  RegExp_prototype_Exec does same
    // thing already.
    if (!IsRegExpObject(R))
        return UnwrapAndCallRegExpBuiltinExec(R, S, forTest);

    // Steps 1-3 (skipped).

    // Step 4.
    var lastIndex = ToLength(R.lastIndex);

    // Step 5.
    var flags = UnsafeGetInt32FromReservedSlot(R, REGEXP_FLAGS_SLOT);

    // Steps 6-7.
    var globalOrSticky = !!(flags & (REGEXP_GLOBAL_FLAG | REGEXP_STICKY_FLAG));

    // Step 8.
    if (!globalOrSticky) {
        lastIndex = 0;
    } else {
        // Step 12.a.
        if (lastIndex > S.length) {
            // Steps 12.a.i-ii.
            if (globalOrSticky)
                R.lastIndex = 0;
            return forTest ? false : null;
        }
    }

    if (forTest) {
        // Steps 3, 9-25, except 12.a.i-ii, 12.c.i.1-2, 15.
        var endIndex = RegExpTester(R, S, lastIndex);
        if (endIndex == -1) {
            // Steps 12.a.i-ii, 12.c.i.1-2.
            if (globalOrSticky)
                R.lastIndex = 0;
            return false;
        }

        // Step 15.
        if (globalOrSticky)
            R.lastIndex = endIndex;

        return true;
    }

    // Steps 3, 9-25, except 12.a.i-ii, 12.c.i.1-2, 15.
    var result = RegExpMatcher(R, S, lastIndex);
    if (result === null) {
        // Steps 12.a.i, 12.c.i.
        if (globalOrSticky)
            R.lastIndex = 0;
    } else {
        // Step 15.
        if (globalOrSticky)
            R.lastIndex = result.index + result[0].length;
    }

    return result;
}

function UnwrapAndCallRegExpBuiltinExec(R, S, forTest) {
    return callFunction(CallRegExpMethodIfWrapped, R, S, forTest, "CallRegExpBuiltinExec");
}

function CallRegExpBuiltinExec(S, forTest) {
    return RegExpBuiltinExec(this, S, forTest);
}

// ES6 21.2.5.13.
function RegExpTest(string) {
    // Steps 1-2.
    var R = this;
    if (!IsObject(R))
        ThrowTypeError(JSMSG_NOT_NONNULL_OBJECT, R === null ? "null" : typeof R);

    // Steps 3-4.
    var S = ToString(string);

    // Steps 5-6.
    return RegExpExec(R, S, true);
}

// ES 2016 draft Mar 25, 2016 21.2.4.2.
function RegExpSpecies() {
    // Step 1.
    return this;
}
_SetCanonicalName(RegExpSpecies, "get [Symbol.species]");
