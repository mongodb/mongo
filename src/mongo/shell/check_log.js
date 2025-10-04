/*
 * Helper functions which connect to a server or reads a log file, and check its logs for particular
 * strings.
 */
function getGlobalLog(connOrFile) {
    assert(typeof connOrFile !== "undefined", "Connection is undefined");
    let cmdRes;
    if (typeof connOrFile === "object") {
        try {
            cmdRes = connOrFile.adminCommand({getLog: "global"});
        } catch (e) {
            // Retry with network errors.
            print("checkLog ignoring failure: " + e);
            return null;
        }
        return assert.commandWorked(cmdRes).log;
    } else {
        assert(typeof connOrFile === "string", "Connection or path to log file must be used");
        try {
            // Attempt to read the file.
            return cat(connOrFile).trim().split("\n");
        } catch (e) {
            print(`Failed to read the file at path: ${connOrFile}. Error: ${tojson(e)}`);
            throw e;
        }
    }
}

/*
 * Calls the 'getLog' function on the provided connection 'conn' to see if the provided msg
 * is found in the logs. Note: this function does not throw an exception, so the return
 * value should not be ignored.
 */
function getLogMessage(connOrFile, msg) {
    const logMessages = getGlobalLog(connOrFile);
    if (logMessages === null) {
        return null;
    }
    if (msg instanceof RegExp) {
        for (let logMsg of logMessages) {
            if (logMsg.search(msg) != -1) {
                return logMsg;
            }
        }
    } else {
        for (let logMsg of logMessages) {
            if (logMsg.includes(msg)) {
                return logMsg;
            }
        }
    }
    return null;
}

/*
 * Calls the 'getLog' function on the provided connection or log file 'connOrFile' to see if the
 * provided msg is found in the logs. Note: this function does not throw an exception, so the
 * return value should not be ignored.
 */
function checkContainsOnce(connOrFile, msg) {
    const logMessages = getGlobalLog(connOrFile);
    if (logMessages === null) {
        return false;
    }
    if (msg instanceof RegExp) {
        for (let logMsg of logMessages) {
            if (logMsg.search(msg) != -1) {
                return true;
            }
        }
    } else {
        for (let logMsg of logMessages) {
            if (logMsg.includes(msg)) {
                return true;
            }
        }
    }
    return false;
}

function checkContainsOnceJson(connOrFile, id, attrsDict, severity = null) {
    const logMessages = getGlobalLog(connOrFile);
    if (logMessages === null) {
        return false;
    }

    for (let logMsg of logMessages) {
        let obj;
        try {
            obj = JSON.parse(logMsg);
        } catch (ex) {
            print("checkLog.checkContainsOnce: JsonJSON.parse() failed: " + tojson(ex) + ": " + logMsg);
            throw ex;
        }

        if (compareLogs(obj, id, severity, null, attrsDict)) {
            return true;
        }
    }

    return false;
}

/*
 * Acts just like checkContainsOnceJson but introduces the `expectedCount`, `isRelaxed`,
 * `comparator`, and `context` params. `isRelaxed` is used to determine how object attributes
 * are handled for matching purposes. If `isRelaxed` is true, then only the fields included in
 * the object in attrsDict will be checked for in the corresponding object in the log.
 * Otherwise, the objects will be checked for complete equality. After counting the total number
 * of logs that match `id` and `attrsDict` based on `isRelaxed`, the `comparator` function is
 * applied to appropriately compare the count with `expectedCount`. `context` allows to check
 * the `ctx` field of the logs.
 */
function checkContainsWithCountJson(
    connOrFile,
    id,
    attrsDict,
    expectedCount,
    severity = null,
    isRelaxed = false,
    comparator = (actual, expected) => {
        return actual === expected;
    },
    context = null,
) {
    const messages = getFilteredLogMessages(connOrFile, id, attrsDict, severity, isRelaxed, context);

    const count = messages.length;

    return comparator(count, expectedCount);
}

/*
 * Similar to checkContainsWithCountJson, but checks whether there are at least 'expectedCount'
 * instances of 'id' in the logs.
 */
function checkContainsWithAtLeastCountJson(
    connOrFile,
    id,
    attrsDict,
    expectedCount,
    severity = null,
    isRelaxed = false,
    context = null,
) {
    return checkContainsWithCountJson(
        connOrFile,
        id,
        attrsDict,
        expectedCount,
        severity,
        isRelaxed,
        (actual, expected) => {
            return actual >= expected;
        },
        context,
    );
}

/*
 * Calls the 'getLog' function on the provided connection or log file 'connOrFile' to see if a
 * log with the provided id is found in the logs. If the id is found it looks up the specified
 * attribute by attrName and checks if the msg is found in its value. Note: this function does
 * not throw an exception, so the return value should not be ignored.
 */
function checkContainsOnceJsonStringMatch(connOrFile, id, attrName, msg) {
    const logMessages = getGlobalLog(connOrFile);
    if (logMessages === null) {
        return false;
    }

    for (let logMsg of logMessages) {
        if (logMsg.search(`"id":${id},`) != -1) {
            if (logMsg.search(`"${attrName}":"?[^"|\\"]*` + msg) != -1) {
                return true;
            }
        }
    }

    return false;
}

/*
 * See checkContainsWithCountJson comment.
 */
function getFilteredLogMessages(connOrFile, id, attrsDict, severity = null, isRelaxed = false, context = null) {
    const logMessages = getGlobalLog(connOrFile);
    if (logMessages === null) {
        return false;
    }

    let messages = [];

    for (let logMsg of logMessages) {
        let obj;
        try {
            obj = JSON.parse(logMsg);
        } catch (ex) {
            print("checkLog.checkContainsOnce: JsonJSON.parse() failed: " + tojson(ex) + ": " + logMsg);
            throw ex;
        }

        if (compareLogs(obj, id, severity, context, attrsDict, isRelaxed)) {
            messages.push(obj);
        }
    }

    return messages;
}

/*
 * Calls the 'getLog' function at regular intervals on the provided connection or log file
 * 'connOrFile' until the provided 'msg' is found in the logs, or it times out. Throws an
 * exception on timeout.
 */
function contains(connOrFile, msg, timeoutMillis = 5 * 60 * 1000, retryIntervalMS = 300) {
    // Don't run the hang analyzer because we don't expect contains() to always succeed.
    assert.soon(
        function () {
            return checkContainsOnce(connOrFile, msg);
        },
        "Could not find log entries containing the following message: " + msg,
        timeoutMillis,
        retryIntervalMS,
        {runHangAnalyzer: false},
    );
}

/*
 * Calls the 'getLog' function at regular intervals on the provided connection or log
 * file'connOrFile' until the provided 'msg' is found in the logs and returned, or it times out.
 * Throws an exception on timeout.
 */
function containsLog(connOrFile, msg, timeoutMillis = 5 * 60 * 1000, retryIntervalMS = 300) {
    // Don't run the hang analyzer because we don't expect contains() to always succeed.
    let logMsg = null;
    assert.soon(
        function () {
            logMsg = getLogMessage(connOrFile, msg);
            if (logMsg) {
                return true;
            }
            return false;
        },
        "Could not find log entries containing the following message: " + msg,
        timeoutMillis,
        retryIntervalMS,
        {runHangAnalyzer: false},
    );
    return logMsg;
}

function containsJson(connOrFile, id, attrsDict, timeoutMillis = 5 * 60 * 1000) {
    // Don't run the hang analyzer because we don't expect contains() to always succeed.
    assert.soon(
        function () {
            return checkContainsOnceJson(connOrFile, id, attrsDict);
        },
        "Could not find log entries containing the following id: " + id + ", and attrs: " + tojson(attrsDict),
        timeoutMillis,
        300,
        {runHangAnalyzer: false},
    );
}

/*
 * Calls checkContainsWithCountJson with the `isRelaxed` parameter set to true at regular
 * intervals on the provided connection or log file 'connOrFile'. It terminates when a log with
 * id 'id', ctx 'context', and all of the attributes in 'attrsDict' is found at least, at most,
 * or exactly `expectedCount` times based on the `comparator` function passed in or the timeout
 * (in ms) is reached.
 */
function containsRelaxedJson(
    connOrFile,
    id,
    attrsDict,
    expectedCount = 1,
    timeoutMillis = 5 * 60 * 1000,
    comparator = (actual, expected) => {
        return actual === expected;
    },
    context = null,
) {
    // Don't run the hang analyzer because we don't expect contains() to always succeed.
    assert.soon(
        function () {
            return checkContainsWithCountJson(
                connOrFile,
                id,
                attrsDict,
                expectedCount,
                null,
                true,
                comparator,
                context,
            );
        },
        "Could not find log entries containing the following id: " + id + ", and attrs: " + tojson(attrsDict),
        timeoutMillis,
        300,
        {runHangAnalyzer: false},
    );
}

/*
 * Calls the 'getLog' function at regular intervals on the provided connection or log file
 * 'connOrFile' until the provided 'msg' is found in the logs 'expectedCount' times, or it times
 * out. Throws an exception on timeout. If 'exact' is true, checks whether the count is exactly
 * equal to 'expectedCount'. Otherwise, checks whether the count is at least equal to
 * 'expectedCount'. Early returns when at least 'expectedCount' entries are found.
 */
function containsWithCount(connOrFile, msg, expectedCount, timeoutMillis = 5 * 60 * 1000, exact = true) {
    let expectedStr = exact ? "exactly " : "at least ";
    assert.soon(
        function () {
            let count = 0;
            let logMessages = getGlobalLog(connOrFile);
            if (logMessages === null) {
                return false;
            }
            for (let i = 0; i < logMessages.length; i++) {
                if (msg instanceof RegExp) {
                    if (logMessages[i].search(msg) != -1) {
                        count++;
                    }
                } else {
                    if (logMessages[i].indexOf(msg) != -1) {
                        count++;
                    }
                }
                if (!exact && count >= expectedCount) {
                    print(
                        "checkLog found at least " +
                            expectedCount +
                            " log entries containing the following message: " +
                            msg,
                    );
                    return true;
                }
            }

            return exact ? expectedCount === count : expectedCount <= count;
        },
        "Did not find " + expectedStr + expectedCount + " log entries containing the " + "following message: " + msg,
        timeoutMillis,
        300,
    );
}

/*
 * Similar to containsWithCount, but checks whether there are at least 'expectedCount'
 * instances of 'msg' in the logs.
 */
function containsWithAtLeastCount(connOrFile, msg, expectedCount, timeoutMillis = 5 * 60 * 1000) {
    containsWithCount(connOrFile, msg, expectedCount, timeoutMillis, /*exact*/ false);
}

/*
 * Converts a scalar or object to a string format suitable for matching against log output.
 * Field names are not quoted, and by default strings which are not within an enclosing
 * object are not escaped. Similarly, integer values without an enclosing object are
 * serialized as integers, while those within an object are serialized as floats to one
 * decimal point. NumberLongs are unwrapped prior to serialization.
 */
function formatAsLogLine(value, escapeStrings, toDecimal) {
    if (typeof value === "string") {
        return escapeStrings ? `"${value}"` : value;
    } else if (typeof value === "number") {
        return Number.isInteger(value) && toDecimal ? value.toFixed(1) : value;
    } else if (value instanceof NumberLong) {
        return `${value}`.match(/NumberLong..(.*)../m)[1];
    } else if (typeof value !== "object") {
        return value;
    } else if (Object.keys(value).length === 0) {
        return Array.isArray(value) ? "[]" : "{}";
    }
    let serialized = [];
    escapeStrings = toDecimal = true;
    for (let fieldName in value) {
        const valueStr = formatAsLogLine(value[fieldName], escapeStrings, toDecimal);
        serialized.push(Array.isArray(value) ? valueStr : `${fieldName}: ${valueStr}`);
    }
    return Array.isArray(value) ? `[ ${serialized.join(", ")} ]` : `{ ${serialized.join(", ")} }`;
}

/**
 * Format at the json for the log file which has no extra spaces.
 */
function formatAsJsonLogLine(value, escapeStrings, toDecimal) {
    if (typeof value === "string") {
        return escapeStrings ? `"${value}"` : value;
    } else if (typeof value === "number") {
        return Number.isInteger(value) && toDecimal ? value.toFixed(1) : value;
    } else if (value instanceof NumberLong) {
        return `${value}`.match(/NumberLong..(.*)../m)[1];
    } else if (typeof value !== "object") {
        return value;
    } else if (Object.keys(value).length === 0) {
        return Array.isArray(value) ? "[]" : "{}";
    }
    let serialized = [];
    escapeStrings = true;
    for (let fieldName in value) {
        const valueStr = formatAsJsonLogLine(value[fieldName], escapeStrings, toDecimal);
        serialized.push(Array.isArray(value) ? valueStr : `"${fieldName}":${valueStr}`);
    }
    return Array.isArray(value) ? `[${serialized.join(",")}]` : `{${serialized.join(",")}}`;
}

/**
 * Internal helper to compare objects filed by field. object1 is considered as the object
 * from the log, while object2 is considered as the expected object from the test. If
 * `isRelaxed` is true, then object1 must contain all of the fields in object2, but can contain
 * other fields as well. By default, this checks for an exact match between object1 and object2.
 */
const _deepEqual = function (object1, object2, isRelaxed = false) {
    if (object1 == null || object2 == null) {
        return false;
    }
    const keys1 = Object.keys(object1);
    const keys2 = Object.keys(object2);

    if (!isRelaxed && keys1.length !== keys2.length) {
        return false;
    }

    for (const key of keys2) {
        const val1 = object1[key];
        const val2 = object2[key];
        // Check if val2 is a regex that needs to be matched.
        if (val2 instanceof RegExp) {
            if (!val2.test(val1)) {
                return false;
            } else {
                continue;
            }
        }
        // If they are any other type of object, then recursively call _deepEqual(). Otherwise
        // perform a simple equality check.
        const areObjects = _isObject(val1) && _isObject(val2);
        if ((areObjects && !_deepEqual(val1, val2, isRelaxed)) || (!areObjects && val1 !== val2)) {
            return false;
        }
    }

    return true;
};

// Internal helper to check that the argument is a non-null object.
const _isObject = function (object) {
    return object != null && typeof object === "object";
};

/*
 * Check if a log's id, severity, and attributes match with what's expected.
 * If `isRelaxed` is true, then the `_deepEqual()` helper function will only check that the
 * fields specified in the attrsDict attribute are equal to those in the corresponding attribute
 * of obj. Otherwise, `_deepEqual()` checks that both subobjects are identical.
 */
function compareLogs(obj, id, severity, context, attrsDict, isRelaxed = false) {
    if (obj.id !== id) {
        return false;
    }
    if (severity !== null && obj.s !== severity) {
        return false;
    }
    if (context !== null) {
        if (context instanceof RegExp) {
            if (!context.test(obj.ctx)) {
                return false;
            }
        } else if (context !== obj.ctx) {
            return false;
        }
    }

    for (let attrKey in attrsDict) {
        const attrValue = attrsDict[attrKey];
        if (attrValue instanceof Function) {
            if (!attrValue(obj.attr[attrKey])) {
                return false;
            }
        } else if (attrValue instanceof RegExp) {
            if (!attrValue.test(obj.attr[attrKey])) {
                return false;
            }
        } else if (obj.attr[attrKey] !== attrValue && typeof obj.attr[attrKey] == "object") {
            if (!_deepEqual(obj.attr[attrKey], attrValue, isRelaxed)) {
                return false;
            }
        } else {
            if (obj.attr[attrKey] !== attrValue) {
                return false;
            }
        }
    }
    return true;
}

export const checkLog = {
    getGlobalLog,
    getLogMessage,
    checkContainsOnce,
    checkContainsOnceJson,
    checkContainsWithCountJson,
    checkContainsWithAtLeastCountJson,
    checkContainsOnceJsonStringMatch,
    compareLogs,
    contains,
    containsLog,
    containsJson,
    containsRelaxedJson,
    containsWithCount,
    containsWithAtLeastCount,
    formatAsLogLine,
    formatAsJsonLogLine,
    getFilteredLogMessages,
};
