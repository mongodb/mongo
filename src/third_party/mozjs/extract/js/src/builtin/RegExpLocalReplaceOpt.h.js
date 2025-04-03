/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// Function template for the following functions:
//   * RegExpLocalReplaceOpt
//   * RegExpLocalReplaceOptFunc
//   * RegExpLocalReplaceOptSubst
// Define the following macro and include this file to declare function:
//   * FUNC_NAME     -- function name (required)
//       e.g.
//         #define FUNC_NAME RegExpLocalReplaceOpt
// Define the following macro (without value) to switch the code:
//   * SUBSTITUTION     -- replaceValue is a string with "$"
//   * FUNCTIONAL       -- replaceValue is a function
//   * SHORT_STRING     -- replaceValue is a string without "$" and lengthS < 0x7fff
//   * neither of above -- replaceValue is a string without "$"

// ES2023 draft rev 2c78e6f6b5bc6bfbf79dd8a12a9593e5b57afcd2
// 22.2.5.11 RegExp.prototype [ @@replace ] ( string, replaceValue )
// Steps 12.a-17.
// Optimized path for @@replace with the following conditions:
//   * global flag is false
function FUNC_NAME(
  rx,
  S,
  lengthS,
  replaceValue,
#ifdef SUBSTITUTION
  firstDollarIndex
#endif
) {
  // 21.2.5.2.2 RegExpBuiltinExec, step 4.
  var lastIndex = ToLength(rx.lastIndex);

  // 21.2.5.2.2 RegExpBuiltinExec, step 5.
  // Side-effects in step 4 can recompile the RegExp, so we need to read the
  // flags again and handle the case when global was enabled even though this
  // function is optimized for non-global RegExps.
  var flags = UnsafeGetInt32FromReservedSlot(rx, REGEXP_FLAGS_SLOT);

  // 21.2.5.2.2 RegExpBuiltinExec, steps 6-7.
  var globalOrSticky = !!(flags & (REGEXP_GLOBAL_FLAG | REGEXP_STICKY_FLAG));

  if (globalOrSticky) {
    // 21.2.5.2.2 RegExpBuiltinExec, step 12.a.
    if (lastIndex > lengthS) {
      if (globalOrSticky) {
        rx.lastIndex = 0;
      }

      // Steps 12-16.
      return S;
    }
  } else {
    // 21.2.5.2.2 RegExpBuiltinExec, step 8.
    lastIndex = 0;
  }

#if !defined(SHORT_STRING)
  // Step 12.a.
  var result = RegExpMatcher(rx, S, lastIndex);

  // Step 12.b.
  if (result === null) {
    // 21.2.5.2.2 RegExpBuiltinExec, steps 12.a.i, 12.c.i.
    if (globalOrSticky) {
      rx.lastIndex = 0;
    }

    // Steps 13-17.
    return S;
  }
#else
  // Step 12.a.
  var result = RegExpSearcher(rx, S, lastIndex);

  // Step 12.b.
  if (result === -1) {
    // 21.2.5.2.2 RegExpBuiltinExec, steps 12.a.i, 12.c.i.
    if (globalOrSticky) {
      rx.lastIndex = 0;
    }

    // Steps 13-17.
    return S;
  }
#endif

  // Steps 12.c, 13-14.

#if !defined(SHORT_STRING)
  // Steps 15.a-b.
  assert(result.length >= 1, "RegExpMatcher doesn't return an empty array");

  // Step 15.c.
  var matched = result[0];

  // Step 15.d.
  var matchLength = matched.length;

  // Step 15.e-f.
  var position = result.index;

  // Step 15.m.iii (reordered)
  // To set rx.lastIndex before RegExpGetFunctionalReplacement.
  var nextSourcePosition = position + matchLength;
#else
  // Steps 15.a-d (skipped).

  // Step 15.e-f.
  var position = result & 0x7fff;

  // Step 15.m.iii (reordered)
  var nextSourcePosition = (result >> 15) & 0x7fff;
#endif

  // 21.2.5.2.2 RegExpBuiltinExec, step 15.
  if (globalOrSticky) {
    rx.lastIndex = nextSourcePosition;
  }

  var replacement;
  // Steps 15.g-l.
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
#else
  replacement = replaceValue;
#endif

  // Step 15.m.ii.
  var accumulatedResult = Substring(S, 0, position) + replacement;

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
