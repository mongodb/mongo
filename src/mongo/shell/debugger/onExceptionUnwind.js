/**
 * Callback for the Debugger Object's onExceptionUnwind function.
 * https://firefox-source-docs.mozilla.org/devtools-user/debugger-api/debugger/index.html#debugger-handler-functions
 */
(function (frame, value) {
    if (globalThis.__fromInteractiveREPL()) {
        // Don't over-process exceptions if this came from an interactive REPL (the stack/content is disconnected to jump to)
        // This might come from the Debug Console, hovering on a variable, at the prompt from a debugger statement, etc.
        return;
    }

    const url = frame.script?.url;
    if (!url) {
        // Shell is terminated or in the process of - don't pause.
        // eg: This can occur from Mochalite exceptions that are running on shell exit,
        // and the final "frame" is back in the C++ execution that invoked the runner.
        return;
    }

    globalThis.__pausedLocation = {
        script: url,
        line: frame.script.getOffsetLocation(frame.offset)?.lineNumber ?? 0,
    };

    // Store the exception value for display
    let exceptionStr = "unknown exception";
    try {
        if (value && value.class !== undefined) {
            const unwrapped = value.unsafeDereference();
            if (unwrapped && unwrapped.message) {
                exceptionStr = (unwrapped.name || "Error") + ": " + unwrapped.message;
            } else {
                exceptionStr = String(unwrapped);
            }
        } else {
            exceptionStr = String(value);
        }
    } catch (e) {
        exceptionStr = "[Could not format exception]";
    }
    globalThis.__storeExceptionInfo(exceptionStr);

    globalThis.__storeCallStack(frame);
    globalThis.__processScopes(frame);
    globalThis.__onException();
    globalThis.__spinwait(frame, {onEval: () => globalThis.__processScopes(frame)});
});
