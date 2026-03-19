/**
 * Shared helpers for SpiderMonkey debugger pause handlers.
 * Evaluated once during initialization; installs shared functions on globalThis.
 */

// Re-applies breakpoints to already-loaded scripts whose breakpoint list changed.
globalThis.__applyPendingBPUpdates = function () {
    const updatedUrls = globalThis.__getBPUpdatedUrls();
    for (const url of updatedUrls) {
        const lines = globalThis.__getBreakpoints(url) ?? [];
        for (const s of globalThis.__dbg.findScripts({url})) {
            // Clear any stale breakpoints before re-applying the current set.
            s.clearAllBreakpoints();
            for (const line of lines) {
                const offsets = s.getLineOffsets(line);
                if (offsets.length > 0) {
                    s.setBreakpoint(offsets[0], {hit: globalThis.__breakpointHitHandler});
                }
            }
        }
    }
};

// Shared spinwait used by all pause handlers (breakpoint hit, exception, debugger statement).
// opts.onEval: optional callback invoked after each eval (eg. to refresh captured scopes).
globalThis.__spinwait = function (frame, opts = {}) {
    while (globalThis.__isPaused()) {
        if (globalThis.__hasBPUpdateRequest()) {
            globalThis.__applyPendingBPUpdates();
        }
        if (globalThis.__hasEvalRequest()) {
            const expr = globalThis.__getEvalRequest();
            const wrappedExpr = `\
                (function() {
                    try {
                        const __result = (${expr});
                        return tojson(__result);
                    } catch (e) {
                        return e.name + ": " + e.message;
                    }
                })()`;
            let result = "";
            try {
                const output = frame.eval(wrappedExpr);
                if (output.return) {
                    result = output.return;
                } else if (output.throw) {
                    const e = output.throw.unsafeDereference();
                    result = e.name + ": " + e.message;
                }
            } catch (e) {
                result = e.name + ": " + e.message;
            }
            globalThis.__storeEvalResult(result);
            opts.onEval?.();
        }
    }
};

// Walks the call stack and stores all frames via the C++ callback.
globalThis.__storeCallStack = function (frame) {
    const stackFrames = [];
    let currentFrame = frame;
    let id = 1;
    while (currentFrame && id <= 50) {
        stackFrames.push({
            url: currentFrame.script?.url ?? "unknown",
            line: currentFrame.script?.getOffsetLocation(currentFrame.offset)?.lineNumber ?? 0,
        });
        currentFrame = currentFrame.older;
        id++;
    }
    globalThis.__storeStackFrames(stackFrames);
};

// Extracts scope/variable info from the current frame and stores it via C++ callbacks.
globalThis.__processScopes = function (frame) {
    globalThis.__storeScopes([]); // clears stale variables on the C++ side

    const scopes = [];
    let scopeId = 1;
    let env = frame.environment;

    while (env) {
        const scopeName = getScopeName(env, scopeId);
        if (scopeName === "Global") {
            // Skip: too many properties, would cause timeouts
            env = env.parent;
            continue;
        }

        scopes.push({name: scopeName, variablesReference: scopeId, expensive: false});

        const variables = [];
        for (const name of env.names()) {
            const debuggerObj = env.getVariable(name);
            variables.push(getVariable(name, debuggerObj, scopeId * 1_000 + variables.length + 1));
        }
        globalThis.__storeVariables(scopeId, variables);

        env = env.parent;
        scopeId++;
    }

    globalThis.__storeScopes(scopes);

    function getScopeName(env, level) {
        switch (env.type) {
            case "declarative":
                return ["Local", "Closure", "Script"][level - 1] ?? "Module";
            case "object":
                return "Global";
            default:
                return "Unknown";
        }
    }

    function getVariable(name, debuggerObj, varRef) {
        if (debuggerObj === null || debuggerObj === undefined) {
            const s = String(debuggerObj);
            return {name, value: s, type: s, variablesReference: 0};
        }
        if (debuggerObj.class !== undefined) {
            const unwrapped = debuggerObj.unsafeDereference();
            const type = typeof unwrapped;
            if (unwrapped === null || unwrapped === undefined) {
                const s = String(unwrapped);
                return {name, value: s, type: s, variablesReference: 0};
            }
            if (type === "string") {
                return {name, value: '"' + unwrapped + '"', type, variablesReference: 0};
            }
            if (type === "object") {
                if (debuggerObj.class === "Array") return formatArrayVariable(unwrapped, debuggerObj, name, varRef);
                return formatObjectVariable(debuggerObj.class, debuggerObj, name, varRef);
            }
            return {name, value: String(unwrapped), type, variablesReference: 0};
        }
        return {name, value: String(debuggerObj), type: typeof debuggerObj, variablesReference: 0};
    }

    function formatArrayVariable(arr, debuggerObj, name, variablesReference) {
        const arrayElements = [];
        for (let i = 0; i < arr.length; i++) {
            const elemValue = debuggerObj.getOwnPropertyDescriptor(String(i))?.value;
            let elemStr = "unknown",
                elemType = "unknown";
            if (elemValue?.class !== undefined) {
                const elemUnwrapped = elemValue.unsafeDereference();
                if (elemValue.class === "Array") {
                    elemStr = `Array(${elemUnwrapped.length})`;
                    elemType = "array";
                } else {
                    elemStr = elemValue.class + " {...}";
                    elemType = "object";
                }
            } else {
                elemStr = String(elemValue);
                elemType = typeof elemValue;
                if (elemType === "string") elemStr = `"${elemValue}"`;
            }
            arrayElements.push({name: `[${i}]`, value: elemStr, type: elemType, variablesReference: 0});
        }
        globalThis.__storeVariables(variablesReference, arrayElements);
        return {name, value: `Array(${arr.length})`, type: "array", variablesReference};
    }

    function formatObjectVariable(className, debuggerObj, name, variablesReference) {
        const objProps = [];
        for (const propName of debuggerObj.getOwnPropertyNames()) {
            const propDesc = debuggerObj.getOwnPropertyDescriptor(propName);
            if (!propDesc?.value) continue;
            const propValue = propDesc.value;
            let propStr = "unknown",
                propType = "unknown";
            if (propValue?.class !== undefined) {
                const propUnwrapped = propValue.unsafeDereference();
                if (propValue.class === "Array") {
                    propStr = "Array(" + propUnwrapped.length + ")";
                    propType = "array";
                } else {
                    propStr = propValue.class + " {...}";
                    propType = "object";
                }
            } else {
                propStr = String(propValue);
                propType = typeof propValue;
                if (propType === "string") propStr = `"${propValue}"`;
            }
            objProps.push({name: propName, value: propStr, type: propType, variablesReference: 0});
        }
        globalThis.__storeVariables(variablesReference, objProps);
        return {name, value: className + " {...}", type: "object", variablesReference};
    }
};
