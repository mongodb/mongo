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

    // Only stop on uncaught exceptions
    if (!isUncaughtInTestContent(frame)) {
        return;
    }

    globalThis.__pausedLocation = {
        script: frame.script?.url,
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

function isUncaughtInTestContent(thrower) {
    if (inFramework(thrower)) {
        // skip framework throwers, regardless of caught or not
        return;
    }

    const catcher = getCatchingFrame(thrower);
    if (!catcher) {
        return true;
    }

    // It will be caught somewhere, but we still consider it "uncaught" if it's the
    // "default" catcher of the underlying framework (the user didn't try to catch this).
    return inFramework(catcher);
}

function inFramework(frame) {
    // Frameworks and test runners have builtin exception handling that wrap the content
    const FRAMEWORK_SCRIPTS = ["mochalite.js"];

    const url = frame.script?.url ?? "";
    return FRAMEWORK_SCRIPTS.some((s) => url.endsWith(s));
}

function getCatchingFrame(frame) {
    let f = frame;
    while (f) {
        if (f.script?.isInCatchScope(f.offset)) {
            return f;
        }
        f = f.older;
    }
    return null;
}
