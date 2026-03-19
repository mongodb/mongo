/**
 * Callback for the Debugger Object's onDebuggerStatement function.
 * https://firefox-source-docs.mozilla.org/devtools-user/debugger-api/debugger/index.html#debugger-handler-functions
 */
(function (frame) {
    globalThis.__pausedLocation = {
        script: frame.script?.url ?? "unknown",
        line: frame.script?.getOffsetLocation(frame.offset)?.lineNumber ?? 0,
    };

    globalThis.__onDebuggerStatement(frame);
    globalThis.__spinwait(frame);

    return undefined;
});
