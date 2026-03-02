/**
 * Callback for the Debugger Object's onDebuggerStatement function.
 * https://firefox-source-docs.mozilla.org/devtools-user/debugger-api/debugger/index.html#debugger-handler-functions
 */
(function (frame) {
    // Store location info so C++ can access it
    globalThis.__pausedLocation = {
        script: frame.script?.url ?? "unknown",
        line: frame.script?.getOffsetLocation(frame.offset)?.lineNumber ?? 0,
    };

    // invoke the C++ callback
    globalThis.__onDebuggerStatement(frame);

    // Spin-wait until the paused flag is cleared
    // This blocks JavaScript execution in this frame
    while (globalThis.__isPaused()) {
        // Check for pending evaluation requests
        if (globalThis.__hasEvalRequest()) {
            // Get the expression to evaluate
            const expr = globalThis.__getEvalRequest();

            // Wrap the expression to format the result with tojson in the debuggee context
            const wrappedExpr = `\
                (function() {
                    try {
                        const __result = (${expr});
                        return tojson(__result);
                    } catch (e) {
                        // eg. reference errors, assertion failures
                        return e.name + ": " + e.message;
                    }
                })()`;
            // Evaluate in the context of the current frame
            let result = "";
            try {
                const output = frame.eval(wrappedExpr);
                if (output.return) {
                    result = output.return;
                } else if (output.throw) {
                    // eg, syntax error in eval'ed string
                    const e = output.throw.unsafeDereference(); // unwrap Debugger.Object
                    result = e.name + ": " + e.message;
                }
            } catch (e) {
                // something really unexpected happened, but avoid a crash
                result = e.name + ": " + e.message;
            }
            globalThis.__storeEvalResult(result);
        }
    }

    return undefined;
});
