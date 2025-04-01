/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// https://github.com/tc39/ecma262/pull/2418 22.2.6.4 get RegExp.prototype.flags
// https://arai-a.github.io/ecma262-compare/?pr=2418&id=sec-get-regexp.prototype.flags
// Uncloned functions with `$` prefix are allocated as extended function
// to store the original name in `SetCanonicalName`.
function $RegExpFlagsGetter() {
  // Steps 1-2.
  var R = this;
  if (!IsObject(R)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, R === null ? "null" : typeof R);
  }

  // Step 3.
  var result = "";

  // Steps 4-5.
  if (R.hasIndices) {
    result += "d";
  }

  // Steps 6-7.
  if (R.global) {
    result += "g";
  }

  // Steps 8-9.
  if (R.ignoreCase) {
    result += "i";
  }

  // Steps 10-11.
  if (R.multiline) {
    result += "m";
  }

  // Steps 12-13.
  if (R.dotAll) {
    result += "s";
  }

  // Steps 14-15.
  if (R.unicode) {
    result += "u";
  }

  // Steps 16-17.
  if (R.unicodeSets) {
    result += "v";
  }

  // Steps 18-19
  if (R.sticky) {
    result += "y";
  }

  // Step 20.
  return result;
}
SetCanonicalName($RegExpFlagsGetter, "get flags");

// ES 2017 draft 40edb3a95a475c1b251141ac681b8793129d9a6d 21.2.5.14.
function $RegExpToString() {
  // Step 1.
  var R = this;

  // Step 2.
  if (!IsObject(R)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, R === null ? "null" : typeof R);
  }

  // Step 3.
  var pattern = ToString(R.source);

  // Step 4.
  var flags = ToString(R.flags);

  // Steps 5-6.
  return "/" + pattern + "/" + flags;
}
SetCanonicalName($RegExpToString, "toString");

// ES 2016 draft Mar 25, 2016 21.2.5.2.3.
function AdvanceStringIndex(S, index) {
  // Step 1.
  assert(typeof S === "string", "Expected string as 1st argument");

  // Step 2.
  assert(
    index >= 0 && index <= MAX_NUMERIC_INDEX,
    "Expected integer as 2nd argument"
  );

  // Step 3 (skipped).

  // Step 4 (skipped).

  // Steps 5-11.
  var supplementary = (
    index < S.length &&
    callFunction(std_String_codePointAt, S, index) > 0xffff
  );
  return index + 1 + supplementary;
}

// ES2023 draft rev 2c78e6f6b5bc6bfbf79dd8a12a9593e5b57afcd2
// 22.2.5.8 RegExp.prototype [ @@match ] ( string )
function RegExpMatch(string) {
  // Step 1.
  var rx = this;

  // Step 2.
  if (!IsObject(rx)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, rx === null ? "null" : typeof rx);
  }

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
    return RegExpBuiltinExec(rx, S);
  }

  // Stes 4-6
  return RegExpMatchSlowPath(rx, S);
}

// ES2023 draft rev 2c78e6f6b5bc6bfbf79dd8a12a9593e5b57afcd2
// 22.2.5.8 RegExp.prototype [ @@match ] ( string )
// Steps 4-6
function RegExpMatchSlowPath(rx, S) {
  // Step 4.
  var flags = ToString(rx.flags);

  // Step 5.
  if (!callFunction(std_String_includes, flags, "g")) {
    return RegExpExec(rx, S);
  }

  // Step 6.a.
  var fullUnicode = callFunction(std_String_includes, flags, "u");

  // Step 6.b.
  rx.lastIndex = 0;

  // Step 6.c.
  var A = [];

  // Step 6.d.
  var n = 0;

  // Step 6.e.
  while (true) {
    // Step 6.e.i.
    var result = RegExpExec(rx, S);

    // Step 6.e.ii.
    if (result === null) {
      return n === 0 ? null : A;
    }

    // Step 6.e.iii.1.
    var matchStr = ToString(result[0]);

    // Step 6.e.iii.2.
    DefineDataProperty(A, n, matchStr);

    // Step 6.e.iii.3.
    if (matchStr === "") {
      var lastIndex = ToLength(rx.lastIndex);
      rx.lastIndex = fullUnicode
        ? AdvanceStringIndex(S, lastIndex)
        : lastIndex + 1;
    }

    // Step 6.e.iii.4.
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
    var position = RegExpSearcher(rx, S, lastIndex);

    // Step 6.e.ii.
    if (position === -1) {
      return n === 0 ? null : A;
    }

    lastIndex = RegExpSearcherLastLimit(S);

    // Step 6.e.iii.1.
    var matchStr = Substring(S, position, lastIndex - position);

    // Step 6.e.iii.2.
    DefineDataProperty(A, n, matchStr);

    // Step 6.e.iii.4.
    if (matchStr === "") {
      lastIndex = fullUnicode
        ? AdvanceStringIndex(S, lastIndex)
        : lastIndex + 1;
      if (lastIndex > lengthS) {
        return A;
      }
    }

    // Step 6.e.iii.5.
    n++;
  }
}

// Checks if following properties and getters are not modified, and accessing
// them not observed by content script:
//   * flags
//   * hasIndices
//   * global
//   * ignoreCase
//   * multiline
//   * dotAll
//   * sticky
//   * unicode
//   * unicodeSets
//   * exec
//   * lastIndex
function IsRegExpMethodOptimizable(rx) {
  if (!IsRegExpObject(rx)) {
    return false;
  }

  var RegExpProto = GetBuiltinPrototype("RegExp");
  // If RegExpPrototypeOptimizable and RegExpInstanceOptimizable succeed,
  // `RegExpProto.exec` is guaranteed to be data properties.
  return (
    RegExpPrototypeOptimizable(RegExpProto) &&
    RegExpInstanceOptimizable(rx, RegExpProto) &&
    RegExpProto.exec === RegExp_prototype_Exec
  );
}

// ES2023 draft rev 2c78e6f6b5bc6bfbf79dd8a12a9593e5b57afcd2
// 22.2.5.11 RegExp.prototype [ @@replace ] ( string, replaceValue )
function RegExpReplace(string, replaceValue) {
  // Step 1.
  var rx = this;

  // Step 2.
  if (!IsObject(rx)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, rx === null ? "null" : typeof rx);
  }

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
    if (replaceValue.length > 1) {
      firstDollarIndex = GetFirstDollarIndex(replaceValue);
    }
  }

  // Optimized paths.
  if (IsRegExpMethodOptimizable(rx)) {
    // Step 7.
    var flags = UnsafeGetInt32FromReservedSlot(rx, REGEXP_FLAGS_SLOT);

    // Step 9.
    var global = !!(flags & REGEXP_GLOBAL_FLAG);

    // Steps 9-17.
    if (global) {
      if (functionalReplace) {
        // For large strings check if the replacer function is
        // applicable for the elem-base optimization.
        if (lengthS > 5000) {
          var elemBase = GetElemBaseForLambda(replaceValue);
          if (IsObject(elemBase)) {
            return RegExpGlobalReplaceOptElemBase(
              rx,
              S,
              lengthS,
              replaceValue,
              flags,
              elemBase
            );
          }
        }
        return RegExpGlobalReplaceOptFunc(rx, S, lengthS, replaceValue, flags);
      }
      if (firstDollarIndex !== -1) {
        return RegExpGlobalReplaceOptSubst(
          rx,
          S,
          lengthS,
          replaceValue,
          flags,
          firstDollarIndex
        );
      }
      return RegExpGlobalReplaceOptSimple(rx, S, lengthS, replaceValue, flags);
    }

    if (functionalReplace) {
      return RegExpLocalReplaceOptFunc(rx, S, lengthS, replaceValue);
    }
    if (firstDollarIndex !== -1) {
      return RegExpLocalReplaceOptSubst(
        rx,
        S,
        lengthS,
        replaceValue,
        firstDollarIndex
      );
    }
    return RegExpLocalReplaceOptSimple(rx, S, lengthS, replaceValue);
  }

  // Steps 7-17.
  return RegExpReplaceSlowPath(
    rx,
    S,
    lengthS,
    replaceValue,
    functionalReplace,
    firstDollarIndex
  );
}

// ES2023 draft rev 2c78e6f6b5bc6bfbf79dd8a12a9593e5b57afcd2
// 22.2.5.11 RegExp.prototype [ @@replace ] ( string, replaceValue )
// Steps 7-17.
// Slow path for @@replace.
function RegExpReplaceSlowPath(
  rx,
  S,
  lengthS,
  replaceValue,
  functionalReplace,
  firstDollarIndex
) {
  // Step 7.
  var flags = ToString(rx.flags);

  // Step 8.
  var global = callFunction(std_String_includes, flags, "g");

  // Step 9.
  var fullUnicode = false;
  if (global) {
    // Step 9.a.
    fullUnicode = callFunction(std_String_includes, flags, "u");

    // Step 9.b.
    rx.lastIndex = 0;
  }

  // Step 10.
  var results = new_List();
  var nResults = 0;

  // Steps 11-12.
  while (true) {
    // Step 12.a.
    var result = RegExpExec(rx, S);

    // Step 12.b.
    if (result === null) {
      break;
    }

    // Step 12.c.i.
    DefineDataProperty(results, nResults++, result);

    // Step 12.c.ii.
    if (!global) {
      break;
    }

    // Step 12.c.iii.1.
    var matchStr = ToString(result[0]);

    // Step 12.c.iii.2.
    if (matchStr === "") {
      var lastIndex = ToLength(rx.lastIndex);
      rx.lastIndex = fullUnicode
        ? AdvanceStringIndex(S, lastIndex)
        : lastIndex + 1;
    }
  }

  // Step 13.
  var accumulatedResult = "";

  // Step 14.
  var nextSourcePosition = 0;

  // Step 15.
  for (var i = 0; i < nResults; i++) {
    result = results[i];

    // Steps 15.a-b.
    var nCaptures = std_Math_max(ToLength(result.length) - 1, 0);

    // Step 15.c.
    var matched = ToString(result[0]);

    // Step 15.d.
    var matchLength = matched.length;

    // Steps 15.e-f.
    var position = std_Math_max(
      std_Math_min(ToInteger(result.index), lengthS),
      0
    );

    var replacement;
    if (functionalReplace || firstDollarIndex !== -1) {
      // Steps 15.g-l.
      replacement = RegExpGetComplexReplacement(
        result,
        matched,
        S,
        position,
        nCaptures,
        replaceValue,
        functionalReplace,
        firstDollarIndex
      );
    } else {
      // Steps 15.g, 15.i, 15.i.iv.
      // We don't need captures array, but ToString is visible to script.
      for (var n = 1; n <= nCaptures; n++) {
        // Steps 15.i.i-ii.
        var capN = result[n];

        // Step 15.i.ii.
        if (capN !== undefined) {
          ToString(capN);
        }
      }

      // Steps 15.j, 15.l.i.
      // We don't need namedCaptures, but ToObject is visible to script.
      var namedCaptures = result.groups;
      if (namedCaptures !== undefined) {
        ToObject(namedCaptures);
      }

      // Step 15.l.ii.
      replacement = replaceValue;
    }

    // Step 15.m.
    if (position >= nextSourcePosition) {
      // Step 15.m.ii.
      accumulatedResult +=
        Substring(S, nextSourcePosition, position - nextSourcePosition) +
        replacement;

      // Step 15.m.iii.
      nextSourcePosition = position + matchLength;
    }
  }

  // Step 16.
  if (nextSourcePosition >= lengthS) {
    return accumulatedResult;
  }

  // Step 17.
  return (
    accumulatedResult +
    Substring(S, nextSourcePosition, lengthS - nextSourcePosition)
  );
}

// ES2023 draft rev 2c78e6f6b5bc6bfbf79dd8a12a9593e5b57afcd2
// 22.2.5.11 RegExp.prototype [ @@replace ] ( string, replaceValue )
// https://tc39.es/ecma262/#sec-regexp.prototype-@@replace
// Steps 15.g-l.
// Calculates functional/substitution replacement from match result.
// Used in the following functions:
//   * RegExpReplaceSlowPath
function RegExpGetComplexReplacement(
  result,
  matched,
  S,
  position,
  nCaptures,
  replaceValue,
  functionalReplace,
  firstDollarIndex
) {
  // Step 15.g.
  var captures = new_List();
  var capturesLength = 0;

  // Step 15.k.i (reordered).
  DefineDataProperty(captures, capturesLength++, matched);

  // Steps 15.h, 15.i, 15.i.v.
  for (var n = 1; n <= nCaptures; n++) {
    // Step 15.i.i.
    var capN = result[n];

    // Step 15.i.ii.
    if (capN !== undefined) {
      capN = ToString(capN);
    }

    // Step 15.i.iii.
    DefineDataProperty(captures, capturesLength++, capN);
  }

  // Step 15.j.
  var namedCaptures = result.groups;

  // Step 15.k.
  if (functionalReplace) {
    // For `nCaptures` <= 4 case, call `replaceValue` directly, otherwise
    // use `std_Function_apply` with all arguments stored in `captures`.
    if (namedCaptures === undefined) {
      switch (nCaptures) {
        case 0:
          return ToString(
            callContentFunction(
              replaceValue,
              undefined,
              SPREAD(captures, 1),
              position,
              S
            )
          );
        case 1:
          return ToString(
            callContentFunction(
              replaceValue,
              undefined,
              SPREAD(captures, 2),
              position,
              S
            )
          );
        case 2:
          return ToString(
            callContentFunction(
              replaceValue,
              undefined,
              SPREAD(captures, 3),
              position,
              S
            )
          );
        case 3:
          return ToString(
            callContentFunction(
              replaceValue,
              undefined,
              SPREAD(captures, 4),
              position,
              S
            )
          );
        case 4:
          return ToString(
            callContentFunction(
              replaceValue,
              undefined,
              SPREAD(captures, 5),
              position,
              S
            )
          );
      }
    }

    // Steps 15.k.ii-vi.
    DefineDataProperty(captures, capturesLength++, position);
    DefineDataProperty(captures, capturesLength++, S);
    if (namedCaptures !== undefined) {
      DefineDataProperty(captures, capturesLength++, namedCaptures);
    }
    return ToString(
      callFunction(std_Function_apply, replaceValue, undefined, captures)
    );
  }

  // Step 15.l.
  if (namedCaptures !== undefined) {
    namedCaptures = ToObject(namedCaptures);
  }
  return RegExpGetSubstitution(
    captures,
    S,
    position,
    replaceValue,
    firstDollarIndex,
    namedCaptures
  );
}

// ES2023 draft rev 2c78e6f6b5bc6bfbf79dd8a12a9593e5b57afcd2
// 22.2.5.11 RegExp.prototype [ @@replace ] ( string, replaceValue )
// https://tc39.es/ecma262/#sec-regexp.prototype-@@replace
// Steps 15.g-k.
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

  // Step 15.j (reordered)
  var namedCaptures = result.groups;

  if (namedCaptures === undefined) {
    switch (nCaptures) {
      case 0:
        return ToString(
          callContentFunction(
            replaceValue,
            undefined,
            SPREAD(result, 1),
            position,
            S
          )
        );
      case 1:
        return ToString(
          callContentFunction(
            replaceValue,
            undefined,
            SPREAD(result, 2),
            position,
            S
          )
        );
      case 2:
        return ToString(
          callContentFunction(
            replaceValue,
            undefined,
            SPREAD(result, 3),
            position,
            S
          )
        );
      case 3:
        return ToString(
          callContentFunction(
            replaceValue,
            undefined,
            SPREAD(result, 4),
            position,
            S
          )
        );
      case 4:
        return ToString(
          callContentFunction(
            replaceValue,
            undefined,
            SPREAD(result, 5),
            position,
            S
          )
        );
    }
  }

  // Steps 15.g-i, 15.k.i-ii.
  var captures = new_List();
  for (var n = 0; n <= nCaptures; n++) {
    assert(
      typeof result[n] === "string" || result[n] === undefined,
      "RegExpMatcher returns only strings and undefined"
    );
    DefineDataProperty(captures, n, result[n]);
  }

  // Step 15.k.iii.
  DefineDataProperty(captures, nCaptures + 1, position);
  DefineDataProperty(captures, nCaptures + 2, S);

  // Step 15.k.iv.
  if (namedCaptures !== undefined) {
    DefineDataProperty(captures, nCaptures + 3, namedCaptures);
  }

  // Steps 15.k.v-vi.
  return ToString(
    callFunction(std_Function_apply, replaceValue, undefined, captures)
  );
}

// ES2023 draft rev 2c78e6f6b5bc6bfbf79dd8a12a9593e5b57afcd2
// 22.2.5.11 RegExp.prototype [ @@replace ] ( string, replaceValue )
// Steps 9.b-17.
// Optimized path for @@replace with the following conditions:
//   * global flag is true
//   * replaceValue is a string without "$"
function RegExpGlobalReplaceOptSimple(rx, S, lengthS, replaceValue, flags) {
  // Step 9.a.
  var fullUnicode = !!(flags & REGEXP_UNICODE_FLAG);

  // Step 9.b.
  var lastIndex = 0;
  rx.lastIndex = 0;

  // Step 13 (reordered).
  var accumulatedResult = "";

  // Step 14 (reordered).
  var nextSourcePosition = 0;

  // Step 12.
  while (true) {
    // Step 12.a.
    var position = RegExpSearcher(rx, S, lastIndex);

    // Step 12.b.
    if (position === -1) {
      break;
    }

    lastIndex = RegExpSearcherLastLimit(S);

    // Step 15.m.ii.
    accumulatedResult +=
      Substring(S, nextSourcePosition, position - nextSourcePosition) +
      replaceValue;

    // Step 15.m.iii.
    nextSourcePosition = lastIndex;

    // Step 12.c.iii.2.
    if (lastIndex === position) {
      lastIndex = fullUnicode
        ? AdvanceStringIndex(S, lastIndex)
        : lastIndex + 1;
      if (lastIndex > lengthS) {
        break;
      }
    }
  }

  // Step 16.
  if (nextSourcePosition >= lengthS) {
    return accumulatedResult;
  }

  // Step 17.
  return (
    accumulatedResult +
    Substring(S, nextSourcePosition, lengthS - nextSourcePosition)
  );
}

// ES2023 draft rev 2c78e6f6b5bc6bfbf79dd8a12a9593e5b57afcd2
// 22.2.5.11 RegExp.prototype [ @@replace ] ( string, replaceValue )
// Steps 7-17.
// Optimized path for @@replace.

// Conditions:
//   * global flag is true
//   * replaceValue is a function
#define FUNC_NAME RegExpGlobalReplaceOptFunc
#define FUNCTIONAL
#include "RegExpGlobalReplaceOpt.h.js"
#undef FUNCTIONAL
#undef FUNC_NAME
/* global RegExpGlobalReplaceOptFunc */

// Conditions:
//   * global flag is true
//   * replaceValue is a function that returns element of an object
#define FUNC_NAME RegExpGlobalReplaceOptElemBase
#define ELEMBASE
#include "RegExpGlobalReplaceOpt.h.js"
#undef ELEMBASE
#undef FUNC_NAME
/* global RegExpGlobalReplaceOptElemBase */

// Conditions:
//   * global flag is true
//   * replaceValue is a string with "$"
#define FUNC_NAME RegExpGlobalReplaceOptSubst
#define SUBSTITUTION
#include "RegExpGlobalReplaceOpt.h.js"
#undef SUBSTITUTION
#undef FUNC_NAME
/* global RegExpGlobalReplaceOptSubst */

// Conditions:
//   * global flag is false
//   * replaceValue is a string without "$"
#define FUNC_NAME RegExpLocalReplaceOptSimple
#define SIMPLE
#include "RegExpLocalReplaceOpt.h.js"
#undef SIMPLE
#undef FUNC_NAME
/* global RegExpLocalReplaceOptSimple */

// Conditions:
//   * global flag is false
//   * replaceValue is a function
#define FUNC_NAME RegExpLocalReplaceOptFunc
#define FUNCTIONAL
#include "RegExpLocalReplaceOpt.h.js"
#undef FUNCTIONAL
#undef FUNC_NAME
/* global RegExpLocalReplaceOptFunc */

// Conditions:
//   * global flag is false
//   * replaceValue is a string with "$"
#define FUNC_NAME RegExpLocalReplaceOptSubst
#define SUBSTITUTION
#include "RegExpLocalReplaceOpt.h.js"
#undef SUBSTITUTION
#undef FUNC_NAME
/* global RegExpLocalReplaceOptSubst */

// ES2017 draft rev 6390c2f1b34b309895d31d8c0512eac8660a0210
// 21.2.5.9 RegExp.prototype [ @@search ] ( string )
function RegExpSearch(string) {
  // Step 1.
  var rx = this;

  // Step 2.
  if (!IsObject(rx)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, rx === null ? "null" : typeof rx);
  }

  // Step 3.
  var S = ToString(string);

  // Step 4.
  var previousLastIndex = rx.lastIndex;

  // Step 5.
  var lastIndexIsZero = SameValue(previousLastIndex, 0);
  if (!lastIndexIsZero) {
    rx.lastIndex = 0;
  }

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
      if (flags & (REGEXP_GLOBAL_FLAG | REGEXP_STICKY_FLAG)) {
        rx.lastIndex = previousLastIndex;
      }
    }

    // Steps 9-10.
    return result;
  }

  return RegExpSearchSlowPath(rx, S, previousLastIndex);
}

// ES2017 draft rev 6390c2f1b34b309895d31d8c0512eac8660a0210
// 21.2.5.9 RegExp.prototype [ @@search ] ( string )
// Steps 6-10.
function RegExpSearchSlowPath(rx, S, previousLastIndex) {
  // Step 6.
  var result = RegExpExec(rx, S);

  // Step 7.
  var currentLastIndex = rx.lastIndex;

  // Step 8.
  if (!SameValue(currentLastIndex, previousLastIndex)) {
    rx.lastIndex = previousLastIndex;
  }

  // Step 9.
  if (result === null) {
    return -1;
  }

  // Step 10.
  return result.index;
}

function IsRegExpSplitOptimizable(rx, C) {
  if (!IsRegExpObject(rx)) {
    return false;
  }

  var RegExpCtor = GetBuiltinConstructor("RegExp");
  if (C !== RegExpCtor) {
    return false;
  }

  var RegExpProto = RegExpCtor.prototype;
  // If RegExpPrototypeOptimizable succeeds, `RegExpProto.exec` is guaranteed
  // to be a data property.
  return (
    RegExpPrototypeOptimizable(RegExpProto) &&
    RegExpInstanceOptimizable(rx, RegExpProto) &&
    RegExpProto.exec === RegExp_prototype_Exec
  );
}

// ES 2017 draft 6859bb9ccaea9c6ede81d71e5320e3833b92cb3e 21.2.5.11.
function RegExpSplit(string, limit) {
  // Step 1.
  var rx = this;

  // Step 2.
  if (!IsObject(rx)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, rx === null ? "null" : typeof rx);
  }

  // Step 3.
  var S = ToString(string);

  // Step 4.
  var C = SpeciesConstructor(rx, GetBuiltinConstructor("RegExp"));

  var optimizable =
    IsRegExpSplitOptimizable(rx, C) &&
    (limit === undefined || typeof limit === "number");

  var flags, unicodeMatching, splitter;
  if (optimizable) {
    // Step 5.
    flags = UnsafeGetInt32FromReservedSlot(rx, REGEXP_FLAGS_SLOT);

    // Steps 6-7.
    unicodeMatching = !!(flags & REGEXP_UNICODE_FLAG);

    // Steps 8-10.
    // If split operation is optimizable, perform non-sticky match.
    if (flags & REGEXP_STICKY_FLAG) {
      var source = UnsafeGetStringFromReservedSlot(rx, REGEXP_SOURCE_SLOT);
      splitter = RegExpConstructRaw(source, flags & ~REGEXP_STICKY_FLAG);
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
    if (callFunction(std_String_includes, flags, "y")) {
      newFlags = flags;
    } else {
      newFlags = flags + "y";
    }

    // Step 10.
    splitter = constructContentFunction(C, C, rx, newFlags);
  }

  // Step 11.
  var A = [];

  // Step 12.
  var lengthA = 0;

  // Step 13.
  var lim;
  if (limit === undefined) {
    lim = MAX_UINT32;
  } else {
    lim = limit >>> 0;
  }

  // Step 15.
  var p = 0;

  // Step 16.
  if (lim === 0) {
    return A;
  }

  // Step 14 (reordered).
  var size = S.length;

  // Step 17.
  if (size === 0) {
    // Step 17.a-b.
    if (optimizable) {
      if (RegExpSearcher(splitter, S, 0) !== -1) {
        return A;
      }
    } else {
      if (RegExpExec(splitter, S) !== null) {
        return A;
      }
    }

    // Step 17.d.
    DefineDataProperty(A, 0, S);

    // Step 17.e.
    return A;
  }

  // Step 18.
  var q = p;

  var optimizableNoCaptures = optimizable && !RegExpHasCaptureGroups(splitter, S);

  // Step 19.
  while (q < size) {
    var e, z;
    if (optimizableNoCaptures) {
      // If there are no capturing groups, avoid allocating the match result
      // object |z| (we set it to null). This is the only difference between
      // this branch and the |if (optimizable)| case below.

      // Step 19.a (skipped).
      // splitter.lastIndex is not used.

      // Steps 19.b-c.
      q = RegExpSearcher(splitter, S, q);
      if (q === -1 || q >= size) {
        break;
      }

      // Step 19.d.i.
      e = RegExpSearcherLastLimit(S);
      z = null;
    } else if (optimizable) {
      // Step 19.a (skipped).
      // splitter.lastIndex is not used.

      // Step 19.b.
      z = RegExpMatcher(splitter, S, q);

      // Step 19.c.
      if (z === null) {
        break;
      }

      // splitter.lastIndex is not updated.
      q = z.index;
      if (q >= size) {
        break;
      }

      // Step 19.d.i.
      e = q + z[0].length;
    } else {
      // Step 19.a.
      splitter.lastIndex = q;

      // Step 19.b.
      z = RegExpExec(splitter, S);

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
    DefineDataProperty(A, lengthA, Substring(S, p, q - p));

    // Step 19.d.iv.4.
    lengthA++;

    // Step 19.d.iv.5.
    if (lengthA === lim) {
      return A;
    }

    // Step 19.d.iv.6.
    p = e;

    if (z !== null) {
      // Steps 19.d.iv.7-8.
      var numberOfCaptures = std_Math_max(ToLength(z.length) - 1, 0);

      // Step 19.d.iv.9.
      var i = 1;

      // Step 19.d.iv.10.
      while (i <= numberOfCaptures) {
        // Steps 19.d.iv.10.a-b.
        DefineDataProperty(A, lengthA, z[i]);

        // Step 19.d.iv.10.c.
        i++;

        // Step 19.d.iv.10.d.
        lengthA++;

        // Step 19.d.iv.10.e.
        if (lengthA === lim) {
          return A;
        }
      }
    }

    // Step 19.d.iv.11.
    q = p;
  }

  // Steps 20-22.
  if (p >= size) {
    DefineDataProperty(A, lengthA, "");
  } else {
    DefineDataProperty(A, lengthA, Substring(S, p, size - p));
  }

  // Step 23.
  return A;
}

// ES6 21.2.5.2.
// NOTE: This is not RegExpExec (21.2.5.2.1).
function RegExp_prototype_Exec(string) {
  // Steps 1-3.
  var R = this;
  if (!IsObject(R) || !IsRegExpObject(R)) {
    return callFunction(
      CallRegExpMethodIfWrapped,
      R,
      string,
      "RegExp_prototype_Exec"
    );
  }

  // Steps 4-5.
  var S = ToString(string);

  // Step 6.
  return RegExpBuiltinExec(R, S);
}

// ES6 21.2.5.13.
function RegExpTest(string) {
  // Steps 1-2.
  var R = this;
  if (!IsObject(R)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, R === null ? "null" : typeof R);
  }

  // Steps 3-4.
  var S = ToString(string);

  // Steps 5-6.
  return RegExpExecForTest(R, S);
}

// ES 2016 draft Mar 25, 2016 21.2.4.2.
function $RegExpSpecies() {
  // Step 1.
  return this;
}
SetCanonicalName($RegExpSpecies, "get [Symbol.species]");

function IsRegExpMatchAllOptimizable(rx, C) {
  if (!IsRegExpObject(rx)) {
    return false;
  }

  var RegExpCtor = GetBuiltinConstructor("RegExp");
  if (C !== RegExpCtor) {
    return false;
  }

  var RegExpProto = RegExpCtor.prototype;
  return (
    RegExpPrototypeOptimizable(RegExpProto) &&
    RegExpInstanceOptimizable(rx, RegExpProto)
  );
}

// String.prototype.matchAll proposal.
//
// RegExp.prototype [ @@matchAll ] ( string )
function RegExpMatchAll(string) {
  // Step 1.
  var rx = this;

  // Step 2.
  if (!IsObject(rx)) {
    ThrowTypeError(JSMSG_OBJECT_REQUIRED, rx === null ? "null" : typeof rx);
  }

  // Step 3.
  var str = ToString(string);

  // Step 4.
  var C = SpeciesConstructor(rx, GetBuiltinConstructor("RegExp"));

  var source, flags, matcher, lastIndex;
  if (IsRegExpMatchAllOptimizable(rx, C)) {
    // Step 5, 9-12.
    source = UnsafeGetStringFromReservedSlot(rx, REGEXP_SOURCE_SLOT);
    flags = UnsafeGetInt32FromReservedSlot(rx, REGEXP_FLAGS_SLOT);

    // Step 6.
    matcher = rx;

    // Step 7.
    lastIndex = ToLength(rx.lastIndex);

    // Step 8 (not applicable for the optimized path).
  } else {
    // Step 5.
    source = "";
    flags = ToString(rx.flags);

    // Step 6.
    matcher = constructContentFunction(C, C, rx, flags);

    // Steps 7-8.
    matcher.lastIndex = ToLength(rx.lastIndex);

    // Steps 9-12.
    flags =
      (callFunction(std_String_includes, flags, "g") ? REGEXP_GLOBAL_FLAG : 0) |
      (callFunction(std_String_includes, flags, "u") ? REGEXP_UNICODE_FLAG : 0);

    // Take the non-optimized path.
    lastIndex = REGEXP_STRING_ITERATOR_LASTINDEX_SLOW;
  }

  // Step 13.
  return CreateRegExpStringIterator(matcher, str, source, flags, lastIndex);
}

// String.prototype.matchAll proposal.
//
// CreateRegExpStringIterator ( R, S, global, fullUnicode )
function CreateRegExpStringIterator(regexp, string, source, flags, lastIndex) {
  // Step 1.
  assert(typeof string === "string", "|string| is a string value");

  // Steps 2-3.
  assert(typeof flags === "number", "|flags| is a number value");

  assert(typeof source === "string", "|source| is a string value");
  assert(typeof lastIndex === "number", "|lastIndex| is a number value");

  // Steps 4-9.
  var iterator = NewRegExpStringIterator();
  UnsafeSetReservedSlot(iterator, REGEXP_STRING_ITERATOR_REGEXP_SLOT, regexp);
  UnsafeSetReservedSlot(iterator, REGEXP_STRING_ITERATOR_STRING_SLOT, string);
  UnsafeSetReservedSlot(iterator, REGEXP_STRING_ITERATOR_SOURCE_SLOT, source);
  UnsafeSetReservedSlot(iterator, REGEXP_STRING_ITERATOR_FLAGS_SLOT, flags | 0);
  UnsafeSetReservedSlot(
    iterator,
    REGEXP_STRING_ITERATOR_LASTINDEX_SLOT,
    lastIndex
  );

  // Step 10.
  return iterator;
}

function IsRegExpStringIteratorNextOptimizable() {
  var RegExpProto = GetBuiltinPrototype("RegExp");
  // If RegExpPrototypeOptimizable succeeds, `RegExpProto.exec` is
  // guaranteed to be a data property.
  return (
    RegExpPrototypeOptimizable(RegExpProto) &&
    RegExpProto.exec === RegExp_prototype_Exec
  );
}

// String.prototype.matchAll proposal.
//
// %RegExpStringIteratorPrototype%.next ( )
function RegExpStringIteratorNext() {
  // Steps 1-3.
  var obj = this;
  if (!IsObject(obj) || (obj = GuardToRegExpStringIterator(obj)) === null) {
    return callFunction(
      CallRegExpStringIteratorMethodIfWrapped,
      this,
      "RegExpStringIteratorNext"
    );
  }

  var result = { value: undefined, done: false };

  // Step 4.
  var lastIndex = UnsafeGetReservedSlot(
    obj,
    REGEXP_STRING_ITERATOR_LASTINDEX_SLOT
  );
  if (lastIndex === REGEXP_STRING_ITERATOR_LASTINDEX_DONE) {
    result.done = true;
    return result;
  }

  // Step 5.
  var regexp = UnsafeGetObjectFromReservedSlot(
    obj,
    REGEXP_STRING_ITERATOR_REGEXP_SLOT
  );

  // Step 6.
  var string = UnsafeGetStringFromReservedSlot(
    obj,
    REGEXP_STRING_ITERATOR_STRING_SLOT
  );

  // Steps 7-8.
  var flags = UnsafeGetInt32FromReservedSlot(
    obj,
    REGEXP_STRING_ITERATOR_FLAGS_SLOT
  );
  var global = !!(flags & REGEXP_GLOBAL_FLAG);
  var fullUnicode = !!(flags & REGEXP_UNICODE_FLAG);

  if (lastIndex >= 0) {
    assert(IsRegExpObject(regexp), "|regexp| is a RegExp object");

    var source = UnsafeGetStringFromReservedSlot(
      obj,
      REGEXP_STRING_ITERATOR_SOURCE_SLOT
    );
    if (
      IsRegExpStringIteratorNextOptimizable() &&
      UnsafeGetStringFromReservedSlot(regexp, REGEXP_SOURCE_SLOT) === source &&
      UnsafeGetInt32FromReservedSlot(regexp, REGEXP_FLAGS_SLOT) === flags
    ) {
      // Step 9 (Inlined RegExpBuiltinExec).
      var globalOrSticky = !!(
        flags &
        (REGEXP_GLOBAL_FLAG | REGEXP_STICKY_FLAG)
      );
      if (!globalOrSticky) {
        lastIndex = 0;
      }

      var match =
        lastIndex <= string.length
          ? RegExpMatcher(regexp, string, lastIndex)
          : null;

      // Step 10.
      if (match === null) {
        // Step 10.a.
        UnsafeSetReservedSlot(
          obj,
          REGEXP_STRING_ITERATOR_LASTINDEX_SLOT,
          REGEXP_STRING_ITERATOR_LASTINDEX_DONE
        );

        // Step 10.b.
        result.done = true;
        return result;
      }

      // Step 11.a.
      if (global) {
        // Step 11.a.i.
        var matchLength = match[0].length;
        lastIndex = match.index + matchLength;

        // Step 11.a.ii.
        if (matchLength === 0) {
          // Steps 11.a.ii.1-3.
          lastIndex = fullUnicode
            ? AdvanceStringIndex(string, lastIndex)
            : lastIndex + 1;
        }

        UnsafeSetReservedSlot(
          obj,
          REGEXP_STRING_ITERATOR_LASTINDEX_SLOT,
          lastIndex
        );
      } else {
        // Step 11.b.i.
        UnsafeSetReservedSlot(
          obj,
          REGEXP_STRING_ITERATOR_LASTINDEX_SLOT,
          REGEXP_STRING_ITERATOR_LASTINDEX_DONE
        );
      }

      // Steps 11.a.iii and 11.b.ii.
      result.value = match;
      return result;
    }

    // Reify the RegExp object.
    regexp = RegExpConstructRaw(source, flags);
    regexp.lastIndex = lastIndex;
    UnsafeSetReservedSlot(obj, REGEXP_STRING_ITERATOR_REGEXP_SLOT, regexp);

    // Mark the iterator as no longer optimizable.
    UnsafeSetReservedSlot(
      obj,
      REGEXP_STRING_ITERATOR_LASTINDEX_SLOT,
      REGEXP_STRING_ITERATOR_LASTINDEX_SLOW
    );
  }

  // Step 9.
  var match = RegExpExec(regexp, string);

  // Step 10.
  if (match === null) {
    // Step 10.a.
    UnsafeSetReservedSlot(
      obj,
      REGEXP_STRING_ITERATOR_LASTINDEX_SLOT,
      REGEXP_STRING_ITERATOR_LASTINDEX_DONE
    );

    // Step 10.b.
    result.done = true;
    return result;
  }

  // Step 11.a.
  if (global) {
    // Step 11.a.i.
    var matchStr = ToString(match[0]);

    // Step 11.a.ii.
    if (matchStr.length === 0) {
      // Step 11.a.ii.1.
      var thisIndex = ToLength(regexp.lastIndex);

      // Step 11.a.ii.2.
      var nextIndex = fullUnicode
        ? AdvanceStringIndex(string, thisIndex)
        : thisIndex + 1;

      // Step 11.a.ii.3.
      regexp.lastIndex = nextIndex;
    }
  } else {
    // Step 11.b.i.
    UnsafeSetReservedSlot(
      obj,
      REGEXP_STRING_ITERATOR_LASTINDEX_SLOT,
      REGEXP_STRING_ITERATOR_LASTINDEX_DONE
    );
  }

  // Steps 11.a.iii and 11.b.ii.
  result.value = match;
  return result;
}

// ES2020 draft rev e97c95d064750fb949b6778584702dd658cf5624
// 7.2.8 IsRegExp ( argument )
function IsRegExp(argument) {
  // Step 1.
  if (!IsObject(argument)) {
    return false;
  }

  // Step 2.
  var matcher = argument[GetBuiltinSymbol("match")];

  // Step 3.
  if (matcher !== undefined) {
    return !!matcher;
  }

  // Steps 4-5.
  return IsPossiblyWrappedRegExpObject(argument);
}
