/**
 * Callback for the Debugger Object's onNewScript function.
 * https://firefox-source-docs.mozilla.org/devtools-user/debugger-api/debugger/index.html#debugger-handler-functions
 *
 * This can be called recursively for the same script "file" due to lazy loading.
 *
 * Once for the top-level script content and offsets, eg:
 *    script = {
 *       url: "jstests/my_test.js",
 *       displayName: undefined,
 *       ...
 *    }
 *
 * Then recursively to explicitly cover functions, and subfunctions, etc., eg:
 *    script = {
 *       url: "jstests/my_test.js",
 *       displayName: "myfunction",
 *       ...
 *    }
 */
(function processScript(script) {
    const url = script.url;
    if (!url) return;

    // Set callbacks via script.setBreakpoint for any pending breakpoints
    const pendingBps = globalThis.__getPendingBreakpoints(url);
    if (!pendingBps || pendingBps.length === 0) return;

    for (const line of pendingBps) {
        const offsets = script.getLineOffsets(line);
        if (offsets?.length > 0) {
            const offset = offsets[0];
            script.setBreakpoint(offset, {
                // eslint-disable-next-line object-shorthand
                hit: function (frame) {
                    // Store location info so C++ can access it
                    globalThis.__pausedLocation = {
                        script: frame.script?.url ?? "unknown",
                        line: frame.script?.getOffsetLocation(frame.offset)?.lineNumber ?? 0,
                    };

                    // Invoke the C++ callback
                    globalThis.__onScriptSetBreakpoint();
                },
            });
        }
    }

    // Process child scripts (nested functions) recursively
    const children = script.getChildScripts();
    for (const child of children) {
        processScript(child);
    }
});
