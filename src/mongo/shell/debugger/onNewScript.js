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
    // SpiderMonkey sets 'this' to the Debugger object for onNewScript handlers.
    // Store it globally so __applyPendingBPUpdates (in helpers.js) can call findScripts().
    globalThis.__dbg ??= this;

    const url = script.url;
    if (!url) return;

    // Register the hit handler globally on first call so __applyPendingBPUpdates can reuse it.
    globalThis.__breakpointHitHandler ??= breakpointHitHandler;

    const pendingBps = globalThis.__getBreakpoints(url);
    if (pendingBps?.length > 0) {
        for (const line of pendingBps) {
            const offsets = script.getLineOffsets(line);
            if (offsets.length > 0) {
                script.setBreakpoint(offsets[0], {hit: breakpointHitHandler});
            }
        }
    }

    // Process child scripts (nested functions) recursively
    for (const child of script.getChildScripts()) {
        processScript.call(globalThis.__dbg, child);
    }

    function breakpointHitHandler(frame) {
        globalThis.__pausedLocation = {
            script: frame.script?.url ?? "unknown",
            line: frame.script?.getOffsetLocation(frame.offset)?.lineNumber ?? 0,
        };

        globalThis.__storeCallStack(frame);
        globalThis.__processScopes(frame);
        globalThis.__onScriptSetBreakpoint();
        globalThis.__spinwait(frame, {onEval: () => globalThis.__processScopes(frame)});
    }
});
