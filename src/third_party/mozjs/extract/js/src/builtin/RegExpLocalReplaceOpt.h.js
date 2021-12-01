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

// ES 2017 draft 6390c2f1b34b309895d31d8c0512eac8660a0210 21.2.5.8
// steps 11.a-16.
// Optimized path for @@replace with the following conditions:
//   * global flag is false
function FUNC_NAME(rx, S, lengthS, replaceValue
#ifdef SUBSTITUTION
                   , firstDollarIndex
#endif
                  )
{
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
            if (globalOrSticky)
                rx.lastIndex = 0;

            // Steps 12-16.
            return S;
        }
    } else {
        // 21.2.5.2.2 RegExpBuiltinExec, step 8.
        lastIndex = 0;
    }

#if !defined(SHORT_STRING)
    // Step 11.a.
    var result = RegExpMatcher(rx, S, lastIndex);

    // Step 11.b.
    if (result === null) {
        // 21.2.5.2.2 RegExpBuiltinExec, steps 12.a.i, 12.c.i.
        if (globalOrSticky)
            rx.lastIndex = 0;

        // Steps 12-16.
        return S;
    }
#else
    // Step 11.a.
    var result = RegExpSearcher(rx, S, lastIndex);

    // Step 11.b.
    if (result === -1) {
        // 21.2.5.2.2 RegExpBuiltinExec, steps 12.a.i, 12.c.i.
        if (globalOrSticky)
            rx.lastIndex = 0;

        // Steps 12-16.
        return S;
    }
#endif

    // Steps 11.c, 12-13.

#if !defined(SHORT_STRING)
    // Steps 14.a-b.
    assert(result.length >= 1, "RegExpMatcher doesn't return an empty array");

    // Step 14.c.
    var matched = result[0];

    // Step 14.d.
    var matchLength = matched.length;

    // Step 14.e-f.
    var position = result.index;

    // Step 14.l.iii (reordered)
    // To set rx.lastIndex before RegExpGetFunctionalReplacement.
    var nextSourcePosition = position + matchLength;
#else
    // Steps 14.a-d (skipped).

    // Step 14.e-f.
    var position = result & 0x7fff;

    // Step 14.l.iii (reordered)
    var nextSourcePosition = (result >> 15) & 0x7fff;
#endif

    // 21.2.5.2.2 RegExpBuiltinExec, step 15.
    if (globalOrSticky)
       rx.lastIndex = nextSourcePosition;

    var replacement;
    // Steps g-j.
#if defined(FUNCTIONAL)
    replacement = RegExpGetFunctionalReplacement(result, S, position, replaceValue);
#elif defined(SUBSTITUTION)
    replacement = RegExpGetSubstitution(result, S, position, replaceValue, firstDollarIndex);
#else
    replacement = replaceValue;
#endif

    // Step 14.l.ii.
    var accumulatedResult = Substring(S, 0, position) + replacement;

    // Step 15.
    if (nextSourcePosition >= lengthS)
        return accumulatedResult;

    // Step 16.
    return accumulatedResult + Substring(S, nextSourcePosition, lengthS - nextSourcePosition);
}
