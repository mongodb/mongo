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

// ES 2017 draft 03bfda119d060aca4099d2b77cf43f6d4f11cfa2 21.2.5.8
// steps 8.b-16.
// Optimized path for @@replace with the following conditions:
//   * global flag is true
function FUNC_NAME(rx, S, lengthS, replaceValue, flags
#ifdef SUBSTITUTION
                   , firstDollarIndex
#endif
#ifdef ELEMBASE
                   , elemBase
#endif
                  )
{
    // Step 8.a.
    var fullUnicode = !!(flags & REGEXP_UNICODE_FLAG);

    // Step 8.b.
    var lastIndex = 0;
    rx.lastIndex = 0;

#if defined(FUNCTIONAL) || defined(ELEMBASE)
    // Save the original source and flags, so we can check if the replacer
    // function recompiled the regexp.
    var originalSource = UnsafeGetStringFromReservedSlot(rx, REGEXP_SOURCE_SLOT);
    var originalFlags = flags;
#endif

    // Step 12 (reordered).
    var accumulatedResult = "";

    // Step 13 (reordered).
    var nextSourcePosition = 0;

    // Step 11.
    while (true) {
        // Step 11.a.
        var result = RegExpMatcher(rx, S, lastIndex);

        // Step 11.b.
        if (result === null)
            break;

        // Steps 14.a-b (skipped).
        assert(result.length >= 1, "RegExpMatcher doesn't return an empty array");

        // Step 14.c.
        var matched = result[0];

        // Step 14.d.
        var matchLength = matched.length | 0;

        // Steps 14.e-f.
        var position = result.index | 0;
        lastIndex = position + matchLength;

        // Steps g-k.
        var replacement;
#if defined(FUNCTIONAL)
        replacement = RegExpGetFunctionalReplacement(result, S, position, replaceValue);
#elif defined(SUBSTITUTION)
        replacement = RegExpGetSubstitution(result, S, position, replaceValue, firstDollarIndex);
#elif defined(ELEMBASE)
        if (IsObject(elemBase)) {
            var prop = GetStringDataProperty(elemBase, matched);
            if (prop !== undefined) {
                assert(typeof prop === "string",
                       "GetStringDataProperty should return either string or undefined");
                replacement = prop;
            } else {
                elemBase = undefined;
            }
        }

        if (!IsObject(elemBase))
            replacement = RegExpGetFunctionalReplacement(result, S, position, replaceValue);
#else
        replacement = replaceValue;
#endif

        // Step 14.l.ii.
        accumulatedResult += Substring(S, nextSourcePosition,
                                       position - nextSourcePosition) + replacement;

        // Step 14.l.iii.
        nextSourcePosition = lastIndex;

        // Step 11.c.iii.2.
        if (matchLength === 0) {
            lastIndex = fullUnicode ? AdvanceStringIndex(S, lastIndex) : lastIndex + 1;
            if (lastIndex > lengthS)
                break;
            lastIndex |= 0;
        }

#if defined(FUNCTIONAL) || defined(ELEMBASE)
        // Ensure the current source and flags match the original regexp, the
        // replaceValue function may have called RegExp#compile.
        if (UnsafeGetStringFromReservedSlot(rx, REGEXP_SOURCE_SLOT) !== originalSource ||
            UnsafeGetInt32FromReservedSlot(rx, REGEXP_FLAGS_SLOT) !== originalFlags)
        {
            rx = regexp_construct_raw_flags(originalSource, originalFlags);
        }
#endif
    }

    // Step 15.
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;

    // Step 16.
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}
