/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// Function template for the following functions:
//   * RegExpGlobalReplaceOpt
//   * RegExpGlobalReplaceOptFunc
//   * RegExpGlobalReplaceOptSubst
//   * RegExpGlobalReplaceOptElemBase
// Define the following macro and include this file to declare function:
//   * FUNC_NAME     -- function name (required)
//       e.g.
//         #define FUNC_NAME RegExpGlobalReplaceOpt
// Define the following macro (without value) to switch the code:
//   * SUBSTITUTION     -- replaceValue is a string with "$"
//   * FUNCTIONAL       -- replaceValue is a function
//   * ELEMBASE         -- replaceValue is a function that returns an element
//                         of an object
//   * none of above    -- replaceValue is a string without "$"

// ES2023 draft rev 2c78e6f6b5bc6bfbf79dd8a12a9593e5b57afcd2
// 22.2.5.11 RegExp.prototype [ @@replace ] ( string, replaceValue )
// steps 9-17.
// Optimized path for @@replace with the following conditions:
//   * global flag is true
function FUNC_NAME(
  rx,
  S,
  lengthS,
  replaceValue,
  flags,
#ifdef SUBSTITUTION
  firstDollarIndex,
#endif
#ifdef ELEMBASE
  elemBase
#endif
) {
  // Step 9.a.
  var fullUnicode = !!(flags & REGEXP_UNICODE_FLAG);

  // Step 9.b.
  var lastIndex = 0;
  rx.lastIndex = 0;

#if defined(FUNCTIONAL) || defined(ELEMBASE)
  // Save the original source and flags, so we can check if the replacer
  // function recompiled the regexp.
  var originalSource = UnsafeGetStringFromReservedSlot(rx, REGEXP_SOURCE_SLOT);
  var originalFlags = flags;
#endif

  // Step 13 (reordered).
  var accumulatedResult = "";

  // Step 14 (reordered).
  var nextSourcePosition = 0;

  // Step 12.
  while (true) {
    // Step 12.a.
    var result = RegExpMatcher(rx, S, lastIndex);

    // Step 12.b.
    if (result === null) {
      break;
    }

    // Steps 15.a-b (skipped).
    assert(result.length >= 1, "RegExpMatcher doesn't return an empty array");

    // Step 15.c.
    var matched = result[0];

    // Step 15.d.
    var matchLength = matched.length | 0;

    // Steps 15.e-f.
    var position = result.index | 0;
    lastIndex = position + matchLength;

    // Steps 15.g-l.
    var replacement;
#if defined(FUNCTIONAL)
    replacement = RegExpGetFunctionalReplacement(
      result,
      S,
      position,
      replaceValue
    );
#elif defined(SUBSTITUTION)
    // Step 15.l.i
    var namedCaptures = result.groups;
    if (namedCaptures !== undefined) {
      namedCaptures = ToObject(namedCaptures);
    }
    // Step 15.l.ii
    replacement = RegExpGetSubstitution(
      result,
      S,
      position,
      replaceValue,
      firstDollarIndex,
      namedCaptures
    );
#elif defined(ELEMBASE)
    if (IsObject(elemBase)) {
      var prop = GetStringDataProperty(elemBase, matched);
      if (prop !== undefined) {
        assert(
          typeof prop === "string",
          "GetStringDataProperty should return either string or undefined"
        );
        replacement = prop;
      } else {
        elemBase = undefined;
      }
    }

    if (!IsObject(elemBase)) {
      replacement = RegExpGetFunctionalReplacement(
        result,
        S,
        position,
        replaceValue
      );
    }
#else
    replacement = replaceValue;
#endif

    // Step 15.m.ii.
    accumulatedResult +=
      Substring(S, nextSourcePosition, position - nextSourcePosition) +
      replacement;

    // Step 15.m.iii.
    nextSourcePosition = lastIndex;

    // Step 12.c.iii.2.
    if (matchLength === 0) {
      lastIndex = fullUnicode
        ? AdvanceStringIndex(S, lastIndex)
        : lastIndex + 1;
      if (lastIndex > lengthS) {
        break;
      }
      lastIndex |= 0;
    }

#if defined(FUNCTIONAL) || defined(ELEMBASE)
    // Ensure the current source and flags match the original regexp, the
    // replaceValue function may have called RegExp#compile.
    if (
      UnsafeGetStringFromReservedSlot(rx, REGEXP_SOURCE_SLOT) !==
        originalSource ||
      UnsafeGetInt32FromReservedSlot(rx, REGEXP_FLAGS_SLOT) !== originalFlags
    ) {
      rx = RegExpConstructRaw(originalSource, originalFlags);
    }
#endif
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
