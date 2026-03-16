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

    // Store exception location info
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

    storeCallStack(frame);
    processScopes(frame);

    // Invoke the C++ callback
    globalThis.__onException();

    spinwait(frame);

    // Spin-wait until the paused flag is cleared
    function spinwait(frame) {
        // This blocks JavaScript execution in this frame
        while (globalThis.__isPaused()) {
            // Check for pending evaluation requests
            if (globalThis.__hasEvalRequest()) {
                // Get the expression to evaluate
                const expr = globalThis.__getEvalRequest();

                // Wrap the expression to format the result
                const wrappedExpr = `\
                    (function() {
                        try {
                            const __result = (${expr});
                            return tojson(__result);
                        } catch (e) {
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

                // Refresh the captured variables after evaluation in case it was updated
                processScopes(frame);
            }
        }
    }

    // HELPER FUNCTIONS to parse variable/scope labeling

    function storeCallStack(frame) {
        // Build the full call stack by walking frame.older
        const stackFrames = [];
        let currentFrame = frame;
        let id = 1;
        while (currentFrame && id <= 50) {
            // Limit to 50 frames to avoid infinite loops
            const url = currentFrame.script?.url ?? "unknown";
            const line = currentFrame.script?.getOffsetLocation(currentFrame.offset)?.lineNumber ?? 0;

            stackFrames.push({url, line});

            currentFrame = currentFrame.older;
            id++;
        }
        globalThis.__storeStackFrames(stackFrames);
    }

    // Extract scope and variable information
    function processScopes(frame) {
        // First, clear any old data
        globalThis.__storeScopes([]);

        const scopes = [];
        let scopeId = 1; // Start from 1, 0 means no nested variables
        let env = frame.environment;

        while (env) {
            const scopeName = getScopeName(env, scopeId);
            const isExpensive = scopeName === "Global";

            // Skip variable extraction for expensive scopes to avoid timeout
            // The global scope has a TON of stuff to sift through
            if (isExpensive) {
                env = env.parent;
                continue;
            }

            scopes.push({
                name: scopeName,
                variablesReference: scopeId,
                expensive: isExpensive,
            });

            // Get variable names for this scope
            const variables = [];
            const names = env.names();
            for (const name of names) {
                const debuggerObj = env.getVariable(name);
                const varRef = scopeId * 1_000 + variables.length + 1;
                let v = getVariable(name, debuggerObj, varRef);
                variables.push(v);
            }

            globalThis.__storeVariables(scopeId, variables);

            env = env.parent;
            scopeId++;
        }

        // Now store the scopes (but don't clear variables this time)
        globalThis.__storeScopes(scopes);
    }

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
        let value = "unknown";
        let type = "unknown";
        let variablesReference = 0;

        if (debuggerObj === null || debuggerObj === undefined) {
            value = type = String(debuggerObj);
        } else if (debuggerObj.class !== undefined) {
            // It's a Debugger.Object wrapper (has a 'class' property)
            const unwrapped = debuggerObj.unsafeDereference();
            type = typeof unwrapped;

            if (unwrapped === null || unwrapped === undefined) {
                value = type = String(unwrapped);
            } else if (type === "string") {
                value = '"' + unwrapped + '"';
            } else if (type === "object") {
                // Get more specific object type
                const className = debuggerObj.class;
                variablesReference = varRef;

                if (className === "Array") {
                    return formatArrayVariable(unwrapped, debuggerObj, name, variablesReference);
                } else {
                    return formatObjectVariable(className, debuggerObj, name, variablesReference);
                }
            } else {
                value = String(unwrapped);
            }
        } else {
            // Primitive value
            value = String(debuggerObj);
            type = typeof debuggerObj;
        }

        return {name, value, type, variablesReference};
    }

    function formatArrayVariable(arr, debuggerObj, name, variablesReference) {
        const value = `Array(${arr.length})`;
        const type = "array";

        // Extract array elements for expansion
        // These could go arbitrarily deep, but for now, we'll just show one-level down
        const arrayElements = [];
        for (let i = 0; i < arr.length; i++) {
            const elemValue = debuggerObj.getOwnPropertyDescriptor(String(i))?.value;
            let elemStr = "unknown";
            let elemType = "unknown";

            if (elemValue?.class !== undefined) {
                // Nested object/array
                const elemUnwrapped = elemValue.unsafeDereference();
                elemType = typeof elemUnwrapped;
                if (elemValue.class === "Array") {
                    elemStr = `Array(${elemUnwrapped.length})`;
                    elemType = "array";
                } else {
                    elemStr = elemValue.class + " {...}";
                    elemType = "object";
                }
            } else {
                // Primitive
                elemStr = String(elemValue);
                elemType = typeof elemValue;
                if (elemType === "string") {
                    elemStr = `"${elemValue}"`;
                }
            }

            arrayElements.push({
                name: `[${i}]`,
                value: elemStr,
                type: elemType,
                variablesReference: 0,
            });
        }

        globalThis.__storeVariables(variablesReference, arrayElements);

        return {name, value, type, variablesReference};
    }

    function formatObjectVariable(className, debuggerObj, name, variablesReference) {
        let value = className + " {...}";
        let type = "object";

        // Extract object properties for expansion
        // These could go arbitrarily deep, but for now, we'll just show one-level down
        const objProps = [];
        const ownProps = debuggerObj.getOwnPropertyNames();
        for (const propName of ownProps) {
            const propDesc = debuggerObj.getOwnPropertyDescriptor(propName);
            if (!propDesc || !propDesc.value) continue;

            const propValue = propDesc.value;
            let propStr = "unknown";
            let propType = "unknown";

            if (propValue?.class !== undefined) {
                // Nested object/array
                const propUnwrapped = propValue.unsafeDereference();
                propType = typeof propUnwrapped;
                if (propValue.class === "Array") {
                    propStr = "Array(" + propUnwrapped.length + ")";
                    propType = "array";
                } else {
                    propStr = propValue.class + " {...}";
                    propType = "object";
                }
            } else {
                // Primitive
                propStr = String(propValue);
                propType = typeof propValue;
                if (propType === "string") {
                    propStr = `"${propValue}"`;
                }
            }

            objProps.push({
                name: propName,
                value: propStr,
                type: propType,
                variablesReference: 0,
            });
        }
        globalThis.__storeVariables(variablesReference, objProps);

        return {
            name,
            value,
            type,
            variablesReference,
        };
    }
});
