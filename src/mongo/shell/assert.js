/**
 * Assertion Library
 */

/**
 * Throws test exception with message.
 *
 * @param {string|Function|object} msg Failure message.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {number| BulkWriteResult | BulkWriteError | WriteResult | {
 *         code?: any,
 *         writeErrors?: any,
 *         errorLabels?: any,
 *         writeConcernError?: any,
 *         writeConcernErrors?: any,
 *         hasWriteConcernError?: any }} [obj] Error object to reference in the exception.
 *
 * @throws {Error}
 *
 * @example
 * switch (scenario.type) {
 *   case 'A':
 *     assert.eq(scenario.result, 1);
 *     break;
 *   case 'B':
 *     assert.eq(scenario.result, 2);
 *     break;
 *   default:
 *     doassert('scenario was not type A or B');
 * }
 */
function doassert(msg, obj) {
    // eval if msg is a function
    if (typeof (msg) == "function")
        msg = msg();

    if (typeof (msg) == "object")
        msg = tojson(msg);

    if (jsTest.options().traceExceptions) {
        if (typeof (msg) == "string" && msg.indexOf("assert") == 0)
            print(new Date().toISOString() + " " + msg);
        else
            print(new Date().toISOString() + " assert: " + msg);
    }

    let ex;
    if (obj) {
        ex = _getErrorWithCode(obj, msg);
    } else {
        ex = Error(msg);
    }
    if (jsTest.options().traceExceptions) {
        print(ex.stack);
    }
    throw ex;
};

/**
 * Sort document object fields.
 *
 * @param doc
 *
 * @returns Sorted document object.
 */
function sortDoc(doc) {
    // Helper to sort the elements of the array
    const sortElementsOfArray = function(arr) {
        if (!arr || arr.constructor != Array)
            return arr;
        return arr.map(el => sortDoc(el));
    };

    // not a container we can sort
    if (!(doc instanceof Object))
        return doc;

    // if it an array, sort the elements
    if (doc.constructor == Array)
        return sortElementsOfArray(doc);

    let fields = Object.keys(doc);
    if (fields.length === 0) {
        return doc;
    }

    let newDoc = {};
    fields.sort();
    for (let i = 0; i < fields.length; i++) {
        const field = fields[i];
        if (doc.hasOwnProperty(field)) {
            let tmp = doc[field];

            if (tmp) {
                // Sort recursively for Arrays and Objects (including bson ones)
                if (tmp.constructor == Array)
                    tmp = sortElementsOfArray(tmp);
                else if (tmp._bson || tmp.constructor == Object)
                    tmp = sortDoc(tmp);
            }
            newDoc[field] = tmp;
        }
    }

    return newDoc;
};

/*
 * This function transforms a given function, 'func', into a function 'safeFunc',
 * where 'safeFunc' matches the behavior of 'func', except that it returns false
 * in any instance where 'func' throws an exception. 'safeFunc' also prints
 * message 'excMsg' upon catching such a thrown exception.
 */
function _convertExceptionToReturnStatus(func, excMsg) {
    const safeFunc = () => {
        try {
            return func();
        } catch (e) {
            print(excMsg + ", exception: " + e);
            return false;
        }
    };
    return safeFunc;
}

/**
 * Format error message by replacing occurrences of '{key}'s in 'msg' with 'value' in [key, value]
 * pairs from 'attr'.
 *
 * @param {string} msg Failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 * @param {Function} [serializeFn] Additional function to serialize 'value' in [key, value] pairs
 *     from 'attr'.
 *
 * @returns Failure message.
 */
function formatErrorMsg(msg, attr = {}, serializeFn = tojson) {
    for (const [key, value] of Object.entries(attr)) {
        msg = msg.replaceAll(`{${key}}`, serializeFn(value));
    }
    return msg;
}

function _processMsg(msg) {
    if (typeof msg === "function") {
        msg = msg();
    }

    if (typeof msg === "object") {
        msg = tojson(msg);
    }
    return msg;
}

function _doassert(msg, prefix, attr) {
    if (TestData?.logFormat === "json") {
        if (attr?.res) {
            // Special handling for command reply a.k.a. 'res' parameters, as it might contain
            // non-printable '_commandObj' and '_mongo' properties.
            attr = {
                ...attr,
                ...(attr.res._commandObj && {originalCommand: attr.res._commandObj}),
                ...(attr.res._mongo && {connection: attr.res._mongo})
            };
        }
        doassert(_buildAssertionMessage(msg, prefix), attr);
    }
    doassert(_buildAssertionMessage(msg, formatErrorMsg(prefix, attr, tojson)), attr?.res);
}

function _validateAssertionMessage(msg, attr) {
    if (msg) {
        if (typeof msg === "function") {
            if (msg.length !== 0) {
                _doassert("msg function cannot expect any parameters.");
            }
        } else if (typeof msg !== "string" && typeof msg !== "object") {
            _doassert("msg parameter must be a string, function or object. Found type: " +
                      typeof (msg));
        }
    }
    if (attr) {
        if (typeof attr !== "object") {
            _doassert("attr parameter must be an object. Found type: " + typeof (attr));
        }
    }
}

function _buildAssertionMessage(msg, prefix) {
    let fullMessage = '';

    if (prefix) {
        fullMessage += prefix;
    }

    if (prefix && msg) {
        fullMessage += ' : ';
    }

    if (msg) {
        fullMessage += _processMsg(msg);
    }

    return fullMessage;
}

/**
 * Assert that a value is true.
 *
 * This is a "truthy" condition test for any object/type, not just booleans (ie, not `=== true`).
 *
 * Consider using more specific methods of the assert module, such as `assert.eq`,
 * which produce richer failure diagnostics.
 *
 * @param {*} value Value under test
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert(coll.drop())
 */
function assert(value, msg, attr) {
    if (arguments.length > 3) {
        _doassert("Too many parameters to assert().");
    }

    _validateAssertionMessage(msg, attr);

    if (value) {
        return;
    }

    _doassert(msg, "assert failed", attr);
};

function _isEq(a, b) {
    if (a == b) {
        return true;
    }

    if ((a != null && b != null) && friendlyEqual(a, b)) {
        return true;
    }

    return false;
}

/**
 * Assert equality.
 *
 * Equality is based on '`==`', with a fallthrough to comparing JSON representations.
 * This is not a strict equality (`===`) assertion.
 *
 * @param {*} a Left-hand side operand
 * @param {*} b Right-hand side operand
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const actual = getValue();
 * const expected = 'foobar';
 * assert.eq(actual, expected);
 */
assert.eq = function(a, b, msg, attr) {
    _validateAssertionMessage(msg, attr);

    if (_isEq(a, b)) {
        return;
    }

    _doassert(msg, `[{a}] and [{b}] are not equal`, {a, b, ...attr});
};

function _isDocEq(a, b) {
    return a === b || bsonUnorderedFieldsCompare(a, b) === 0;
}

/**
 * Assert equality of document objects.
 *
 * The order of fields (properties) within objects is ignored.
 * The bsonUnorderedFieldsCompare function is leveraged.
 *
 * @param docA Document object
 * @param docB Document object
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied or the object representation in BSON exceeds
 *     16793600 bytes.
 *
 * @example
 * assert.docEq(results[0], {_id: null, result: ["abc"]})
 */
assert.docEq = function(expectedDoc, actualDoc, msg, attr) {
    _validateAssertionMessage(msg, attr);

    if (_isDocEq(expectedDoc, actualDoc)) {
        return;
    }

    _doassert(msg,
              "expected document {expectedDoc} and actual document {actualDoc} are not equal",
              {expectedDoc, actualDoc, ...attr});
};

/**
 * Assert set equality, for primitive (non-object) set element types.
 *
 * @param setA
 * @param setB
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.setEq(new Set([7, 8, 9, 10]), new Set(matchingIds))
 */
assert.setEq = function(expectedSet, actualSet, msg, attr) {
    _validateAssertionMessage(msg, attr);

    const failAssertion = function() {
        _doassert(
            msg,
            "expected set {expectedSet} and actual set {actualSet} are not equal",
            {expectedSet: Array.from(expectedSet), actualSet: Array.from(actualSet), ...attr});
    };
    if (expectedSet.size !== actualSet.size) {
        failAssertion();
    }
    for (let a of expectedSet) {
        if (!actualSet.has(a)) {
            failAssertion();
        }
    }
};

/**
 * Assert that array have the same members.
 *
 * Order of the elements is ignored.
 *
 * @param {any[]} aArr
 * @param {any[]} bArr
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {Function} [compareFn] Custom element-comparison function
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.sameMembers(res, [{_id: 1, count: 500}, {_id: 2, count: 5001}]);
 */
assert.sameMembers = function(aArr, bArr, msg, compareFn = _isDocEq, attr) {
    _validateAssertionMessage(msg, attr);

    const failAssertion = function() {
        _doassert(msg, "{aArr} != {bArr}", {aArr, bArr, compareFn: compareFn.name, ...attr});
    };

    if (aArr.length !== bArr.length) {
        failAssertion();
    }

    // Keep a set of which indices we've already used to avoid double counting values.
    const matchedIndicesInRight = new Set();
    for (let a of aArr) {
        let foundMatch = false;
        for (let i = 0; i < bArr.length; ++i) {
            // Sort both inputs in case either is a document. Note: by default this does not
            // sort any nested arrays.
            if (!matchedIndicesInRight.has(i) && compareFn(a, bArr[i])) {
                matchedIndicesInRight.add(i);
                foundMatch = true;
                break;
            }
        }
        if (!foundMatch) {
            failAssertion();
        }
    }
};

/**
 * Assert that document arrays have the same members, within a tolerance.
 *
 * Order of the elements is ignored.
 *
 * @param aArr
 * @param bArr
 * @param {string[]} fuzzyFields Fields whose values should be "close".
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {number} [places] Number of decimal places to match. Default 4.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.fuzzySameMembers(
    res, [{"title": "King Kong", "score": 0.802}], ["score"]);
 */
assert.fuzzySameMembers = function(aArr, bArr, fuzzyFields, msg, places = 4, attr) {
    function fuzzyCompare(docA, docB) {
        return _fieldsClose(docA, docB, fuzzyFields, msg, places);
    }
    return assert.sameMembers(aArr, bArr, msg, fuzzyCompare, attr);
};

/**
 * Assert inequality.
 *
 * Inequality is based on '`a != b`', with a fallthrough to comparing JSON representations.
 * This is not a strict equality (`a !== b`) assertion.
 *
 * @param a Left-hand side operand
 * @param b Right-hand side operand
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const actual = getValue();
 * const forbidden = 'foobar';
 * assert.neq(actual, forbidden);
 */
assert.neq = function(a, b, msg, attr) {
    _validateAssertionMessage(msg, attr);

    if (!_isEq(a, b)) {
        return;
    }

    _doassert(msg, "[{a}] and [{b}] are equal", {a, b, ...attr});
};

/**
 * Assert that an object has specific fields.
 *
 * @param {object} obj
 * @param {string[]} arr Array of fields that the object must have.
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.hasFields(result.serverInfo, ['host', 'port', 'version', 'gitVersion']);
 */
assert.hasFields = function(obj, arr, msg, attr) {
    if (!Array.isArray(arr)) {
        _doassert("The second argument to assert.hasFields must be an array.");
    }

    let count = 0;
    for (let field in obj) {
        if (arr.includes(field)) {
            count += 1;
        }
    }

    if (count != arr.length) {
        _doassert(msg, "Not all of the values from {arr} were in {obj}", {obj, arr, ...attr});
    }
};

/**
 * Assert that an array contains a specific element.
 *
 * @param element Element to be found
 * @param {any[]} arr Array to search
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.contains(res.nMatched, [0, 1]);
 */
assert.contains = function(element, arr, msg, attr) {
    if (!Array.isArray(arr)) {
        _doassert("The second argument to assert.contains() must be an array.");
    }

    for (let i = 0; i < arr.length; i++) {
        const comp = arr[i];
        const satisfied =
            comp == element || ((comp != null && element != null) && friendlyEqual(comp, element));
        if (satisfied) {
            return;
        }
    }

    _doassert(msg, "{element} was not in {arr}", {element, arr, ...attr});
};

/**
 * Assert that an array does not contain a specific element.
 *
 * @param element Element to not be found
 * @param {any[]} arr Array to search
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.doesNotContain(errorCode, [401, 404, 500]);
 */
assert.doesNotContain = function(element, arr, msg, attr) {
    if (!Array.isArray(arr)) {
        _doassert("The second argument to assert.doesNotContain must be an array.");
    }

    for (let i = 0; i < arr.length; i++) {
        const comp = arr[i];
        const match =
            comp == element || ((comp != null && element != null) && friendlyEqual(comp, element));
        if (match) {
            _doassert(msg, "{element} is in {arr}", {element, arr, ...attr});
        }
    }
};

/**
 * Assert that an array contains a string that starts with a prefix.
 *
 * @param {string} prefix
 * @param {any[]} arr Array to search
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.containsPrefix("Detected a time-series bucket with mixed schema data", res.warnings);
 */
assert.containsPrefix = function(prefix, arr, msg, attr) {
    if (typeof (prefix) !== "string") {
        _doassert("The first argument to containsPrefix must be a string.");
    }
    if (!Array.isArray(arr)) {
        _doassert("The second argument to containsPrefix must be an array.");
    }

    for (let i = 0; i < arr.length; i++) {
        if (typeof (arr[i]) !== "string") {
            continue;
        }

        const satisfied = arr[i].startsWith(prefix);
        if (satisfied) {
            return;
        }
    }

    _doassert(msg, "{prefix} was not a prefix in {arr}", {prefix, arr, ...attr});
};

/**
 * Assert that a function eventually evaluates to true.
 *
 * Calls a function 'func' at repeated intervals until either func() returns true
 * or more than 'timeout' milliseconds have elapsed.
 *
 * @param {Function} func Function to be executed, or string to be `eval`ed.
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {number} [timeout] Timeout in ms. In CI, this is 10min, otherwise 90sec.
 * @param {number} [interval] Interval in ms to wait between tries, default 200ms.
 * @param {{runHangAnalyzer: boolean}} [opts] Options to control hang analyzer via `runHangAnalyzer`
 *     property.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.soon(() => changeStream.hasNext());
 */
assert.soon = function(func, msg, timeout, interval = 200, {runHangAnalyzer = true} = {}, attr) {
    _validateAssertionMessage(msg, attr);

    const start = new Date();

    if (TestData?.inEvergreen) {
        timeout ??= 10 * 60 * 1_000;  // 10 min
    } else {
        timeout ??= 90 * 1_000;  // 90 sec
    }

    let msgPrefix = `assert.soon failed (timeout ${timeout}ms): ${func}`;
    if (msg && typeof (msg) != "function") {
        msgPrefix = `assert.soon failed (timeout ${timeout}ms), msg`;
    }

    while (1) {
        if (typeof (func) == "string") {
            if (eval(func))
                return;
        } else {
            if (func())
                return;
        }

        const diff = (new Date()).getTime() - start.getTime();
        if (diff > timeout) {
            msg = _buildAssertionMessage(msg);
            if (runHangAnalyzer) {
                msg = msg + " The hang analyzer is automatically called in assert.soon functions." +
                    " If you are *expecting* assert.soon to possibly fail, call assert.soon" +
                    " with {runHangAnalyzer: false} as the fifth argument" +
                    " (you can fill unused arguments with `undefined`).";
                print(msg + " Running hang analyzer from assert.soon.");
                MongoRunner.runHangAnalyzer();
            }
            _doassert(msg, msgPrefix, attr);
        } else {
            sleep(interval);
        }
    }
};

/**
 * Assert that a function eventually evaluates to true.
 *
 * This is a special case of {@link assert.soon}.
 *
 * Calls a function 'func' at repeated intervals until either func() returns true
 * or more than 'timeout' milliseconds have elapsed. Exceptions are allowed and suppressed.
 *
 * @param {Function} func Function to be executed, or string to be `eval`ed.
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {number} [timeout] Timeout in ms. In CI, this is 10min, otherwise 90sec.
 * @param {number} [interval] Interval in ms to wait between tries, default 200ms.
 * @param {{runHangAnalyzer: boolean}} [opts] Options to control hang analyzer via `runHangAnalyzer`
 *     property.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.soonNoExcept(() => {
 *    let numIndices = secondaryDB.getCollection(collectionName).getIndexes().length;
 *    // this might fail/throw a few times, but that is okay
 *    assert.eq(numIndices, 4);
 *    return true;
 * });
 */
assert.soonNoExcept = function(func, msg, timeout, interval, {runHangAnalyzer = true} = {}, attr) {
    const safeFunc = _convertExceptionToReturnStatus(func, "assert.soonNoExcept caught exception");
    const getFunc = () => {
        // No TestData means not running from resmoke. Non-resmoke tests usually don't trace
        // exceptions.
        if (typeof TestData === "undefined") {
            return safeFunc;
        }
        return () => {
            // Turns off printing the JavaScript stacktrace in doassert() to avoid
            // generating an overwhelming amount of log messages when handling transient
            // errors.
            const origTraceExceptions = TestData.traceExceptions;
            TestData.traceExceptions = false;

            const res = safeFunc();

            // Restore it's value to original value.
            TestData.traceExceptions = origTraceExceptions;
            return res;
        };
    };

    assert.soon(getFunc(), msg, timeout, interval, {runHangAnalyzer}, attr);
};

/**
 * Assert that a function eventually evaluates to true.
 *
 * This calls a function up to a specified number of times, whereas {@link assert.soon}
 * calls the function until a timeout is exceeded.
 *
 * @param {Function} func Function to be executed.
 * @param {string|Function|object} msg Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {number} num_attempts Number of attempts to try the function execution.
 * @param {number} [intervalMS] Interval in ms to wait between tries, default 0.
 * @param {{runHangAnalyzer: boolean}} [opts] Options to control hang analyzer via `runHangAnalyzer`
 *     property.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.retry(
 *     () => db.serverStatus().metrics.cursor.open.pinned == 0,
 *     "Expected 0 pinned cursors, but have " + tojson(db.serverStatus().metrics.cursor),
 *     10);
 */
assert.retry = function(
    func, msg, num_attempts, intervalMS = 0, {runHangAnalyzer = true} = {}, attr) {
    let attempts_made = 0;
    while (attempts_made < num_attempts) {
        if (func()) {
            return;
        } else {
            attempts_made += 1;
            print("assert.retry failed on attempt " + attempts_made + " of " + num_attempts);
            sleep(intervalMS);
        }
    }
    // Used up all attempts
    msg = _buildAssertionMessage(msg);
    if (runHangAnalyzer) {
        msg = msg + " The hang analyzer is automatically called in assert.retry functions. " +
            "If you are *expecting* assert.soon to possibly fail, call assert.retry " +
            "with {runHangAnalyzer: false} as the fifth argument " +
            "(you can fill unused arguments with `undefined`).";
        print(msg + " Running hang analyzer from assert.retry.");
        MongoRunner.runHangAnalyzer();
    }
    _doassert(msg, null, attr);
};

/**
 * Assert that a function eventually evaluates to true, ignoring exceptions.
 *
 * Special case of {@link assert.retry} where the function is executed "safely" within
 * a try/catch to continue retries.
 *
 * @param {Function} func Function to be executed.
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {number} [num_attempts] Number of attempts to try the function execution.
 * @param {number} [intervalMS] Interval in ms to wait between tries, default 0.
 * @param {{runHangAnalyzer: boolean}} [opts] Options to control hang analyzer via `runHangAnalyzer`
 *     property.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.retryNoExcept(() => {
 *     assert.commandWorked(configDB.chunks.update({_id: chunkDoc._id}, {$set: {jumbo: true}}));
 *     return true;
 * }, "Setting jumbo flag update failed on config server", 10);
 */
assert.retryNoExcept = function(
    func, msg, num_attempts, intervalMS, {runHangAnalyzer = true} = {}, attr) {
    const safeFunc = _convertExceptionToReturnStatus(func, "assert.retryNoExcept caught exception");
    assert.retry(safeFunc, msg, num_attempts, intervalMS, {runHangAnalyzer}, attr);
};

/**
 * Asserts that a command run on the 'admin' database worked, ignoring network errors.
 *
 * Returns the response if the command succeeded, or undefined if the command failed, *even* if
 * the failure was due to a network error.
 *
 * @param {DB} node
 * @param {object} commandObj Command object to be called inside `node.adminCommand`.
 *
 * @returns The result object to continue any chaining.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.adminCommandWorkedAllowingNetworkError(replTest.getPrimary(), {replSetReconfig: config});
 */
assert.adminCommandWorkedAllowingNetworkError = function(node, commandObj) {
    let res;
    try {
        res = node.adminCommand(commandObj);
        assert.commandWorked(res);
    } catch (e) {
        // Ignore errors due to connection failures.
        if (!isNetworkError(e)) {
            throw e;
        }
        print("Caught network error: " + tojson(e));
    }
    return res;
};

/**
 * Assert that function execution completes within a specified timeout.
 *
 * @param {Function} f Function to be executed, or string to be `eval`ed.
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {number} [timeout] Timeout in ms, default 30_000.
 * @param {{runHangAnalyzer: boolean}} [opts] Options to control hang analyzer via `runHangAnalyzer`
 *     property.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @returns Result of the function evaluation/execution.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * let ok = assert.time(() => {
 *     const testDB = db.getMongo().getDB('test');
 *     const res = testDB.runCommand(
 *         {usersInfo: user.userName, maxTimeMS: 30_000});
 *     return res.ok;
 *  });
 *  assert(ok);
 */
assert.time = function(f, msg, timeout = 30_000 /*ms*/, {runHangAnalyzer = true} = {}, attr) {
    _validateAssertionMessage(msg, attr);

    const start = new Date();

    let res;
    if (typeof (f) == "string") {
        res = eval(f);
    } else {
        res = f();
    }

    const diff = (new Date()).getTime() - start.getTime();
    if (diff <= timeout) {
        return res;
    }

    const msgPrefix = "assert.time failed";
    msg = _buildAssertionMessage(msg);
    if (runHangAnalyzer) {
        msg = msg + " The hang analyzer is automatically called in assert.time functions. " +
            "If you are *expecting* assert.soon to possibly fail, call assert.time " +
            "with {runHangAnalyzer: false} as the fourth argument " +
            "(you can fill unused arguments with `undefined`).";
        print(msg + " Running hang analyzer from assert.time.");
        MongoRunner.runHangAnalyzer();
    }
    _doassert(msg, msgPrefix, {timeMS: diff, timeoutMS: timeout, function: f, diff, ...attr});
};

function assertThrowsHelper(func, params) {
    if (typeof func !== "function") {
        _doassert('1st argument must be a function');
    }

    if (arguments.length >= 2 && !Array.isArray(params) &&
        Object.prototype.toString.call(params) !== "[object Arguments]") {
        _doassert("2nd argument must be an Array or Arguments object");
    }

    let thisKeywordWasUsed = false;

    const thisSpy = new Proxy({}, {
        has: () => {
            thisKeywordWasUsed = true;
            return false;
        },

        get: () => {
            thisKeywordWasUsed = true;
            return undefined;
        },

        set: () => {
            thisKeywordWasUsed = true;
            return false;
        },

        deleteProperty: () => {
            thisKeywordWasUsed = true;
            return false;
        }
    });

    let error = null;
    let res = null;
    try {
        res = func.apply(thisSpy, params);
    } catch (e) {
        error = e;
    }

    if (thisKeywordWasUsed) {
        _doassert("Attempted to access 'this' during function call in" +
                  " assert.throws/doesNotThrow. Instead, wrap the function argument in" +
                  " another function.");
    }

    return {error: error, res: res};
}

/**
 * Assert that a function throws an exception.
 *
 * @param {Function} func Function to be executed.
 * @param {any[]} [params] Parameters to apply into the function execution.
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @returns {Error} that the function threw.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.throws(() => local.aggregate(pipeline));
 */
assert.throws = function(func, params, msg, attr) {
    _validateAssertionMessage(msg, attr);

    // Use .apply() instead of calling the function directly with explicit arguments to
    // preserve the length of the `arguments` object.
    const {error} = assertThrowsHelper.apply(null, arguments);

    if (!error) {
        _doassert(msg, "did not throw exception", attr);
    }

    return error;
};

/**
 * Assert that a function throws an exception matching a specific error code.
 *
 * This is an extension of {@link assert.throws}.
 *
 * @param {Function} func Function to be executed.
 * @param {number | number[]} expectedCode Code (or array of possible Codes) to match on
 *     the `code` field of the resulting error.
 * @param {any[]} [params] Parameters to apply into the function execution.
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @returns {Error} that the function threw.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.throwsWithCode(() => coll.aggregate({$project: {x: "$$ref"}}).toArray(), 17276);
 */
assert.throwsWithCode = function(func, expectedCode, params, msg, attr) {
    if (arguments.length < 2) {
        _doassert("assert.throwsWithCode expects at least 2 arguments");
    }
    // Remove the 'expectedCode' parameter, and any undefined parameters, from the list of
    // arguments. Use .apply() to preserve the length of the 'arguments' object.
    const newArgs = [func, params, msg, attr].filter(element => element !== undefined);
    const error = assert.throws.apply(null, newArgs);
    if (!Array.isArray(expectedCode)) {
        expectedCode = [expectedCode];
    }
    if (!expectedCode.some((ec) => error.code == ec)) {
        _doassert(msg,
                  `[{code}] and [{expectedCode}] are not equal`,
                  {code: error.code, expectedCode, ...attr});
    }
    return error;
};

/**
 * Assert that a function does not throw an exception.
 *
 * This is typically used when the test wants to verify that a function executes safely,
 * but does not warrant any further verifications of its output or effects.
 *
 * @param {Function} func Function to be executed.
 * @param {any[]} [params] Parameters to apply into the function execution.
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @returns The output of the function.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.doesNotThrow(() => source.aggregate(pipeline, options));
 */
assert.doesNotThrow = function(func, params, msg, attr) {
    _validateAssertionMessage(msg, attr);

    // Use .apply() instead of calling the function directly with explicit arguments to
    // preserve the length of the `arguments` object.
    const {error, res} = assertThrowsHelper.apply(null, arguments);

    if (error) {
        const {code, message} = error;
        _doassert(msg,
                  "threw unexpected exception: {error}",
                  {error: {...(code && {code}), ...(message && {message})}, ...attr});
    }

    return res;
};

/**
 * Assert that a function throws an exception matching a specific error code,
 * and trigger a callback function with those as inputs.
 *
 * This is an extension of {@link assert.throwsWithCode}.
 *
 * @param {Function} func Function to be executed.
 * @param {number | number[]} dropCodes Code (or array of Codes) that are expected
 *     to be thrown.
 * @param {Function} onDrop Function to execute on matched {@link dropCodes}.
 *
 * @returns The output of {@onDrop}, invoked with exceptions matching {@link dropCodes}
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.dropExceptionsWithCode(
 *     () => runBackgroundDbCheck(hosts),
 *     ErrorCodes.Interrupted,
 *     (e) => jsTestLog("Skipping dbCheck due to transient error: " + tojson(e)));
 */
assert.dropExceptionsWithCode = function(func, dropCodes, onDrop) {
    if (typeof func !== "function") {
        _doassert('assert.dropExceptionsWithCode 1st argument must be a function');
    }
    if (typeof onDrop !== "function") {
        _doassert('assert.dropExceptionsWithCode 3rd argument must be a function');
    }
    if (!Array.isArray(dropCodes)) {
        dropCodes = [dropCodes];
    }

    try {
        return func();
    } catch (e) {
        if (dropCodes.some((ec) => e.code === ec)) {
            return onDrop(e);
        } else {
            throw e;
        }
    }
};

function _rawReplyOkAndNoWriteErrors(raw, {ignoreWriteErrors, ignoreWriteConcernErrors} = {}) {
    if (raw.ok === 0) {
        return false;
    }

    // A write command response may have ok:1 but write errors.
    if (!ignoreWriteErrors && raw.writeErrors?.length > 0) {
        return false;
    }

    if (!ignoreWriteConcernErrors && raw.hasOwnProperty("writeConcernError")) {
        return false;
    }
    return true;
}

// Returns whether res is a type which may have write errors (not command errors).
// These types imply that the write command succeeded.
function _isWriteResultType(res) {
    return res instanceof WriteResult || res instanceof WriteError ||
        res instanceof BulkWriteResult || res instanceof BulkWriteError;
}

function _validateCommandResponse(res, assertionName) {
    if (typeof res !== "object") {
        _doassert(`unexpected result type given to assert.${assertionName}()`,
                  `expected result type 'object', got '{resultType}', res='{result}'`,
                  {result: res, resultType: typeof res});
    }
}

function _runHangAnalyzerIfWriteConcernTimedOut(res) {
    const timeoutMsg = "waiting for replication timed out";
    let isWriteConcernTimeout = false;
    if (_isWriteResultType(res)) {
        if (res.hasWriteConcernError() && res.getWriteConcernError().errmsg === timeoutMsg) {
            isWriteConcernTimeout = true;
        }
    } else if (res?.errmsg === timeoutMsg || res.writeConcernError?.errmsg === timeoutMsg) {
        isWriteConcernTimeout = true;
    }
    if (isWriteConcernTimeout) {
        print("Running hang analyzer for writeConcern timeout " + tojson(res));
        MongoRunner.runHangAnalyzer();
        return true;
    }
    return false;
}

function _runHangAnalyzerIfNonTransientLockTimeoutError(res) {
    // Concurrency suites see a lot of LockTimeouts when running concurrent transactions.
    // However, they will also abort transactions and continue running rather than fail the
    // test, so we don't want to run the hang analyzer when the error has a
    // TransientTransactionError error label.
    const isTransientTxnError = res.errorLabels?.includes("TransientTransactionError");
    const isLockTimeout = res?.code === ErrorCodes.LockTimeout;
    if (isLockTimeout && !isTransientTxnError) {
        print("Running hang analyzer for lock timeout " + tojson(res));
        MongoRunner.runHangAnalyzer();
        return true;
    }
    return false;
}

function _runHangAnalyzerForSpecificFailureTypes(res) {
    // If the hang analyzer is run, then we shouldn't try to run it again.
    if (_runHangAnalyzerIfWriteConcernTimedOut(res)) {
        return;
    }

    _runHangAnalyzerIfNonTransientLockTimeoutError(res);
}

function _assertCommandWorked(res, msg, {ignoreWriteErrors, ignoreWriteConcernErrors}) {
    _validateAssertionMessage(msg);
    _validateCommandResponse(res, "commandWorked");

    // Keep this as a function so we don't call tojson if not necessary.
    const makeFailPrefix = (res) => {
        let prefix = "command failed: {res}";
        if (typeof res._commandObj === "object" && res._commandObj !== null) {
            prefix += " with original command request: {originalCommand}";
        }
        if (typeof res._mongo === "object" && res._mongo !== null) {
            prefix += " on connection: {connection}";
        }
        if (res.hasOwnProperty("errmsg")) {
            prefix += ` with errmsg: ${res.errmsg}`;
        }
        return prefix;
    };

    if (_isWriteResultType(res)) {
        // These can only contain write errors, not command errors.
        if (!ignoreWriteErrors) {
            assert.writeOK(res, msg, {ignoreWriteConcernErrors: ignoreWriteConcernErrors});
        }
    } else if (res instanceof WriteCommandError || res instanceof Error) {
        // A WriteCommandError implies ok:0.
        // Error objects may have a `code` property added (e.g.
        // DBCollection.prototype.mapReduce) without a `ok` property.
        _doassert(msg,
                  makeFailPrefix(res),
                  {res, originalCommand: res._commandObj, connection: res._mongo});
    } else if (res.hasOwnProperty("ok")) {
        // Handle raw command responses or cases like MapReduceResult which extend command
        // response.
        if (!_rawReplyOkAndNoWriteErrors(res, {
                ignoreWriteErrors: ignoreWriteErrors,
                ignoreWriteConcernErrors: ignoreWriteConcernErrors
            })) {
            _runHangAnalyzerForSpecificFailureTypes(res);
            _doassert(msg,
                      makeFailPrefix(res),
                      {res, originalCommand: res._commandObj, connection: res._mongo});
        }
    } else if (res.hasOwnProperty("acknowledged")) {
        // CRUD api functions return plain js objects with an acknowledged property.
        // no-op.
    } else {
        _doassert(msg, "unknown type of result, cannot check ok: {res}", {res});
    }
    return res;
}

assert._kAnyErrorCode = Object.create(null);

function _assertCommandFailed(res, expectedCode, msg) {
    _validateAssertionMessage(msg);
    _validateCommandResponse(res, "commandFailed");

    if (expectedCode !== assert._kAnyErrorCode && !Array.isArray(expectedCode)) {
        expectedCode = [expectedCode];
    }

    // Keep this as a function so we don't call tojson if not necessary.
    const makeFailPrefix = (res) => {
        if (res.hasOwnProperty("errmsg")) {
            return `command worked when it should have failed: {res}. errmsg: ${res.errMsg}`;
        }
        return "command worked when it should have failed: {res}";
    };

    const makeFailCodePrefix = (res, expectedCode) => {
        if (res.hasOwnProperty("errmsg")) {
            return (expectedCode !== assert._kAnyErrorCode)
                ? `command did not fail with any of the following codes {expectedCode} {res}. errmsg: ${
                      res.errmsg}`
                : null;
        }
        return (expectedCode !== assert._kAnyErrorCode)
            ? "command did not fail with any of the following codes {expectedCode} {res}"
            : null;
    };

    if (_isWriteResultType(res)) {
        // These can only contain write errors, not command errors.
        assert.writeErrorWithCode(res, expectedCode, msg);
    } else if (res instanceof WriteCommandError || res instanceof Error) {
        // A WriteCommandError implies ok:0.
        // Error objects may have a `code` property added (e.g.
        // DBCollection.prototype.mapReduce) without a `ok` property.
        if (expectedCode !== assert._kAnyErrorCode) {
            if (!res.hasOwnProperty("code") || !expectedCode.includes(res.code)) {
                _doassert(msg, makeFailCodePrefix(res, expectedCode), {res, expectedCode});
            }
        }
    } else if (res.hasOwnProperty("ok")) {
        // Handle raw command responses or cases like MapReduceResult which extend command
        // response.
        if (_rawReplyOkAndNoWriteErrors(res)) {
            _doassert(msg, makeFailPrefix(res), {res});
        }

        if (expectedCode !== assert._kAnyErrorCode) {
            let foundCode = false;
            if (res.hasOwnProperty("code") && expectedCode.includes(res.code)) {
                foundCode = true;
            } else if (res.hasOwnProperty("writeErrors")) {
                foundCode = res.writeErrors.some((err) => expectedCode.includes(err.code));
            } else if (res.hasOwnProperty("writeConcernError")) {
                foundCode = expectedCode.includes(res.writeConcernError.code);
            }

            if (!foundCode) {
                _runHangAnalyzerForSpecificFailureTypes(res);
                _doassert(msg, makeFailCodePrefix(res, expectedCode), {res, expectedCode});
            }
        }
    } else if (res.hasOwnProperty("acknowledged")) {
        // CRUD api functions return plain js objects with an acknowledged property.
        _doassert(msg, makeFailPrefix(res), {res});
    } else {
        _doassert(msg, "unknown type of result, cannot check error: {res}", {res});
    }
    return res;
}

/**
 * Assert that a command worked, or otherwise failed with a specific code.
 *
 * This is a convenience wrapper around {@link assert.commandWorked}
 * and {@link assert.commandFailedWithCode}.
 *
 * @param {WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError} res
 *     Result that should be successful ("worked").
 * @param {number | number[]} errorCodeSet Code (or array of possible Codes) to match on failed
 *     results.
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 *
 * @returns The result object to continue any chaining.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const dbTest = db.getSiblingDB(jsTestName());
 * const res = dbTest.createCollection("coll1");
 * assert.commandWorkedOrFailedWithCode(res. 58712);
 */
assert.commandWorkedOrFailedWithCode = function(res, errorCodeSet, msg) {
    try {
        // First check if the command worked.
        return assert.commandWorked(res, msg);
    } catch (e) {
        // If the command did not work, assert it failed with one of the specified codes.
        return assert.commandFailedWithCode(res, errorCodeSet, msg);
    }
};

/**
 * Assert that a command worked, ignoring write concern errors or specific failure codes.
 *
 * This is an extension of {@link assert.commandWorked} and {@link assert.commandFailedWithCode}.
 *
 * @param {WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError} res
 *     Result that should be successful ("worked").
 * @param {number | number[]} [errorCodeSet] Code (or array of possible Codes) to match on failed
 *     results.
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 *
 * @returns The result object to continue any chaining.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const dbTest = db.getSiblingDB(jsTestName());
 * const res = dbTest.createCollection("coll1");
 * assert.commandWorkedIgnoringWriteErrors(res);
 */
assert.commandWorkedIgnoringWriteConcernErrorsOrFailedWithCode = function(res, errorCodeSet, msg) {
    try {
        // First check if the command worked.
        return _assertCommandWorked(res, msg, {ignoreWriteConcernErrors: true});
    } catch (e) {
        // If the command did not work, assert it failed with one of the specified codes.
        return assert.commandFailedWithCode(res, errorCodeSet, msg);
    }
};

/**
 * Assert that a command worked by testing a result object.
 *
 * @param {WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError} res
 *     Result that should be successful ("worked").
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 *
 * @returns The result object to continue any chaining.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const dbTest = db.getSiblingDB(jsTestName());
 * const res = dbTest.createCollection("coll1");
 * assert.commandWorked(res);
 */
assert.commandWorked = function(res, msg) {
    return _assertCommandWorked(res, msg, {ignoreWriteErrors: false});
};

/**
 * Assert that a command worked, ignoring write errors.
 *
 * This is an extension of {@link assert.commandWorked}.
 *
 * @param {WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError} res
 *     Result that should be successful ("worked").
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 *
 * @returns The result object to continue any chaining.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const dbTest = db.getSiblingDB(jsTestName());
 * const res = dbTest.createCollection("coll1");
 * assert.commandWorkedIgnoringWriteErrors(res);
 */
assert.commandWorkedIgnoringWriteErrors = function(res, msg) {
    return _assertCommandWorked(res, msg, {ignoreWriteErrors: true});
};

/**
 * Assert that a command worked, ignoring write concern errors.
 *
 * @param {WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError} res
 *     Result that should be successful ("worked").
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 *
 * @returns The result object to continue any chaining.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const dbTest = db.getSiblingDB(jsTestName());
 * const res = dbTest.createCollection("coll1");
 * assert.commandWorkedIgnoringWriteConcernErrors(res);
 */
assert.commandWorkedIgnoringWriteConcernErrors = function(res, msg) {
    return _assertCommandWorked(res, msg, {ignoreWriteConcernErrors: true});
};

/**
 * Assert that a command worked, ignoring write errors and write concern errors.
 *
 * @param {WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError} res
 *     Result that should be successful ("worked").
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 *
 * @returns The result object to continue any chaining.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const dbTest = db.getSiblingDB(jsTestName());
 * const res = dbTest.createCollection("coll1");
 * assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
 */
assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors = function(res, msg) {
    return _assertCommandWorked(
        res, msg, {ignoreWriteConcernErrors: true, ignoreWriteErrors: true});
};

/**
 * Assert that a command failed.
 *
 * @param {WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError} res
 *     Result that should be successful ("worked").
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 *
 * @returns The result object to continue any chaining.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const dbTest = db.getSiblingDB(jsTestName());
 * const res = dbTest.createCollection("coll1");
 * assert.commandFailed(res);
 */
assert.commandFailed = function(res, msg) {
    return _assertCommandFailed(res, assert._kAnyErrorCode, msg);
};

/**
 * Assert that a command failed with a specific code.
 *
 * @param {WriteResult | BulkWriteResult |  WriteCommandError | WriteError | BulkWriteError} res
 *     Result that should have failed.
 * @param {number | number[]} expectedCode Code (or array of possible Codes) to match
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 *
 * @returns The result object to continue any chaining.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const dbTest = db.getSiblingDB(jsTestName());
 * const res = dbTest.createCollection("coll1");
 * assert.commandFailedWithCode(res, 17260);
 */
assert.commandFailedWithCode = function(res, expectedCode, msg) {
    return _assertCommandFailed(res, expectedCode, msg);
};

/**
 * Assert that a command resulted in successful writes.
 *
 * @param {WriteResult | BulkWriteResult | WriteCommandError | WriteError | BulkWriteError} res
 *     Result object.
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @returns The result object to continue any chaining.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const dbTest = db.getSiblingDB(jsTestName());
 * const res = dbTest.createCollection("coll1");
 * assert.writeOK(res);
 */
assert.writeOK = function(res, msg, {ignoreWriteConcernErrors} = {}) {
    let errMsg = null;

    if (res instanceof WriteResult) {
        if (res.hasWriteError()) {
            errMsg = "write failed with error";
        } else if (!ignoreWriteConcernErrors && res.hasWriteConcernError()) {
            errMsg = "write concern failed with errors";
        }
    } else if (res instanceof BulkWriteResult) {
        // Can only happen with bulk inserts
        if (res.hasWriteErrors()) {
            errMsg = "write failed with errors";
        } else if (!ignoreWriteConcernErrors && res.hasWriteConcernError()) {
            errMsg = "write concern failed with errors";
        }
    } else if (res instanceof WriteCommandError || res instanceof WriteError ||
               res instanceof BulkWriteError) {
        errMsg = "write command failed";
    } else {
        if (!res || !res.ok) {
            errMsg = "unknown type of write result, cannot check ok";
        }
    }

    if (errMsg) {
        _runHangAnalyzerForSpecificFailureTypes(res);
        _doassert(msg, errMsg + ": {res}", {res}, errMsg);
    }

    return res;
};

/**
 * Assert that a command resulted in write errors.
 *
 * @param {WriteResult | BulkWriteResult | WriteCommandError | WriteError | BulkWriteError} res
 *     Result object.
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 *
 * @returns The result object to continue any chaining.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const dbTest = db.getSiblingDB(jsTestName());
 * const res = dbTest.createCollection("coll1");
 * assert.writeError(res);
 */
assert.writeError = function(res, msg) {
    return assert.writeErrorWithCode(res, assert._kAnyErrorCode, msg);
};

/**
 * Assert that a command resulted in write errors matching specific Codes.
 *
 * This is a stricter check of {@link assert.writeError}.
 *
 * @param {WriteResult | BulkWriteResult | WriteCommandError | WriteError | BulkWriteError } res
 *     Result object.
 * @param {number | number[]} expectedCode Code (or array of possible Codes) to match
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 *
 * @returns The result object to continue any chaining.
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const dbTest = db.getSiblingDB(jsTestName());
 * const res = dbTest.createCollection("coll1");
 * assert.writeErrorWithCode(ex, ErrorCodes.DatabaseDropPending);
 */
assert.writeErrorWithCode = function(res, expectedCode, msg) {
    if (expectedCode === undefined) {
        _doassert("assert.writeErrorWithCode called with undefined error code");
    }

    let errMsg = null;
    const writeErrorCodes = new Set();

    if (res instanceof WriteResult) {
        if (res.hasWriteError()) {
            writeErrorCodes.add(res.getWriteError().code);
        } else if (res.hasWriteConcernError()) {
            writeErrorCodes.add(res.getWriteConcernError().code);
        } else {
            errMsg = "no write error";
        }
    } else if (res instanceof BulkWriteResult || res instanceof BulkWriteError) {
        // Can only happen with bulk inserts
        if (res.hasWriteErrors()) {
            // Save every write error code.
            res.getWriteErrors().forEach((we) => writeErrorCodes.add(we.code));
        } else if (res.hasWriteConcernError()) {
            writeErrorCodes.add(res.getWriteConcernError().code);
        } else {
            errMsg = "no write errors";
        }
    } else if (res instanceof WriteCommandError) {
        // Can only happen with bulk inserts
        // No-op since we're expecting an error
    } else if (res instanceof WriteError) {
        writeErrorCodes.add(res.code);
    } else {
        if (!res || res.ok) {
            errMsg = "unknown type of write result, cannot check error";
        }
    }

    if (errMsg) {
        _runHangAnalyzerForSpecificFailureTypes(res);
        _doassert(msg, errMsg + ": {res}", {res});
    }

    if (expectedCode !== assert._kAnyErrorCode) {
        if (!Array.isArray(expectedCode)) {
            expectedCode = [expectedCode];
        }
        const found = expectedCode.some((ec) => writeErrorCodes.has(ec));
        if (!found) {
            errMsg =
                "found code(s) {writeErrorCodes} does not match any of the expected codes {expectedCode}. Original command response: {res}";
            _runHangAnalyzerForSpecificFailureTypes(res);
            _doassert(
                msg, errMsg, {res, expectedCode, writeErrorCodes: Array.from(writeErrorCodes)});
        }
    }

    return res;
};

/**
 * Assert that a value is null.
 *
 * @param value Value under test
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const value = getValue();
 * assert.isnull(value);
 */
assert.isnull = function(value, msg, attr) {
    _validateAssertionMessage(msg, attr);

    if (value == null) {
        return;
    }

    _doassert(msg, "supposed to be null, was: {value}", {value, ...attr});
};

function _shouldUseBsonWoCompare(a, b) {
    const bsonTypes = [
        Timestamp,
    ];

    if (typeof a !== "object" || typeof b !== "object") {
        return false;
    }

    for (let t of bsonTypes) {
        if (a instanceof t && b instanceof t) {
            return true;
        }
    }

    return false;
}

function _compare(f, a, b) {
    if (_shouldUseBsonWoCompare(a, b)) {
        const result = bsonWoCompare({_: a}, {_: b});
        return f(result, 0);
    }

    return f(a, b);
}

function _assertCompare(f, a, b, description, msg, attr) {
    _validateAssertionMessage(msg, attr);

    if (_compare(f, a, b)) {
        return;
    }

    _doassert(msg, "{a} is not " + description + " {b}", {a, b, ...attr});
}

/**
 * Assert that a < b.
 *
 * @param a Left-hand side operand
 * @param b Right-hand side operand
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const actual = getValue();
 * const ceiling = 12;
 * assert.lt(actual, ceiling);
 */
assert.lt = function(a, b, msg, attr) {
    _assertCompare((a, b) => {
        return a < b;
    }, a, b, "less than", msg, attr);
};

/**
 * Assert that a > b.
 *
 * @param a Left-hand side operand
 * @param b Right-hand side operand
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const actual = getValue();
 * const floor = 12;
 * assert.gt(actual, floor);
 */
assert.gt = function(a, b, msg, attr) {
    _assertCompare((a, b) => {
        return a > b;
    }, a, b, "greater than", msg, attr);
};

/**
 * Assert that a <= b.
 *
 * @param a Left-hand side operand
 * @param b Right-hand side operand
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const actual = getValue();
 * const ceiling = 12;
 * assert.lte(actual, ceiling);
 */
assert.lte = function(a, b, msg, attr) {
    _assertCompare((a, b) => {
        return a <= b;
    }, a, b, "less than or eq", msg, attr);
};

/**
 * Assert that a >= b.
 *
 * @param a Left-hand side operand
 * @param b Right-hand side operand
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * const actual = getValue();
 * const floor = 12;
 * assert.gte(actual, floor);
 */
assert.gte = function(a, b, msg, attr) {
    _assertCompare((a, b) => {
        return a >= b;
    }, a, b, "greater than or eq", msg, attr);
};

/**
 * Assert that a <= b <= c, or a < b < c.
 *
 * @param a
 * @param b
 * @param c
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {boolean} [inclusive] Whether to use inclusive (<=) comparisons, default true.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.between(1_000, totalSpillingStats.spilledBytes, 100_000);
 */
assert.between = function(a, b, c, msg, inclusive = true, attr) {
    _validateAssertionMessage(msg, attr);

    let compareFn = (a, b) => {
        return inclusive ? a <= b : a < b;
    };

    if (_compare(compareFn, a, b) && _compare(compareFn, b, c)) {
        return;
    }

    _doassert(msg, "{b} is not between {a} and {c}", {a, b, c, inclusive, ...attr});
};

/**
 * Assert that a <= b <= c, inclusively.
 *
 * This is a convenience wrapper around {@link assert.between}.
 *
 * @param a
 * @param b
 * @param c
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.between(1000, totalSpillingStats.spilledBytes, 100000);
 */
assert.betweenIn = function(a, b, c, msg, attr) {
    assert.between(a, b, c, msg, true, attr);
};
/**
 * Assert that a < b < c, exclusively.
 *
 * This is a convenience wrapper around {@link assert.between}.
 *
 * @param a
 * @param b
 * @param c
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.between(1000, totalSpillingStats.spilledBytes, 100000);
 */
assert.betweenEx = function(a, b, c, msg, attr) {
    assert.between(a, b, c, msg, false, attr);
};

// Returns an array [isClose, msg] where 'isClose' is a bool indiciating whether or not values
// 'a' and 'b' are sufficiently close, and, if they're not, 'msg' is set to a descriptive error
// string.
function _isClose(a, b, places = 4) {
    const absoluteError = Math.abs(a - b);
    if (Math.round(absoluteError * Math.pow(10, places)) === 0) {
        return [true, null];
    }
    // This treats 'places' as significant figures.
    const relativeError = Math.abs(absoluteError / b);
    if (Math.round(relativeError * Math.pow(10, places)) === 0) {
        return [true, null];
    }
    const msgPrefix = `${a} is not equal to ${b} within ${places} places, absolute error: ` +
        `${absoluteError}, relative error: ${relativeError}`;
    return [false, msgPrefix];
}

/**
 * Assert that numerical values are equivalent to within significant figures.
 *
 * @param {number} a
 * @param {number} b
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {number} [places] Number of significant figures to allow for tolerance, default 4.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.close(res.pop, popExpected, '', 10);
 */
assert.close = function(a, b, msg, places = 4) {
    const [isClose, errMsg] = _isClose(a, b, places);
    if (!isClose) {
        _doassert(msg, errMsg);
    }
};

// Given the names of numerical fuzzyFields check that:
//  - For each fuzzyField: if it exists in both docA and docB, those values are 'close'
//  - All other fields are equal between docA and docB.
function _fieldsClose(docA, docB, fuzzyFields, places = 4) {
    let exactSubsets = {a: {}, b: {}};
    for (let currField of Object.keys(docA)) {
        if (docB.hasOwnProperty(currField)) {
            if (fuzzyFields.includes(currField)) {
                if (!_isClose(parseFloat(docA[currField]), parseFloat(docB[currField]), places)) {
                    return false;
                }
            } else {
                exactSubsets.a[currField] = docA[currField];
                exactSubsets.b[currField] = docB[currField];
            }
        }
    }
    for (let currField of Object.keys(docB)) {
        if (!docA.hasOwnProperty(currField)) {
            return false;
        }
    }
    return _isDocEq(exactSubsets.a, exactSubsets.b);
}

/**
 * Asserts if the times in millis are equal to within a tolerance.
 *
 * @param {Date | number} a
 * @param {Date | number} b
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {number} [deltaMS] Tolerance in milliseconds, default 1000 ms.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.closeWithinMS(startTime,
 *                     latestStartUpLog.startTime,
 *                     "StartTime doesn't match one from _id",
 *                     2000); // Expect less than 2 sec delta
 */
assert.closeWithinMS = function(a, b, msg, deltaMS = 1_000, attr) {
    const aMS = a instanceof Date ? a.getTime() : a;
    const bMS = b instanceof Date ? b.getTime() : b;
    const actualDelta = Math.abs(Math.abs(aMS) - Math.abs(bMS));

    if (actualDelta <= deltaMS) {
        return;
    }

    const msgPrefix = "" + a + " is not equal to " + b + " within " + deltaMS + " millis, " +
        "actual delta: " + actualDelta + " millis";

    const forLog = (arg) => arg instanceof Date ? JSON.parse(JSON.stringify(arg)) : arg;
    _doassert(msg, msgPrefix, {a: forLog(a), b: forLog(b), deltaMS, ...attr});
};

/**
 * Assert that the "haystack" includes the "needle".
 *
 * This defers to the builtin "includes" method of the "haystack" object,
 * which might be an array (for element containment), string (for substring-matching), etc.
 *
 * @param haystack
 * @param needle
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.includes(res.message, "$merge failed due to a DuplicateKey error");
 */
assert.includes = function(haystack, needle, msg, attr) {
    if (haystack.includes(needle)) {
        return;
    }

    const prefix = "string [{haystack}] does not include [{needle}]";
    _doassert(msg, prefix, {haystack, needle, ...attr});
};

/**
 * Assert that a command object does not have any of the fields
 * `apiVersion`, `apiStruct`, or `apiDeprecationErrors`.
 *
 * @param {object} cmdOptions Object to validate.
 *                   If it is not an Object, the assertion passes.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.noAPIParams(options);
 */
assert.noAPIParams = function(cmdOptions) {
    if (!(cmdOptions instanceof Object)) {
        return;
    }
    assert(!cmdOptions.hasOwnProperty("apiVersion") && !cmdOptions.hasOwnProperty("apiStrict") &&
               !cmdOptions.hasOwnProperty("apiDeprecationErrors"),
           "API parameters are not allowed in this context");
};

/**
 * Assert that a function eventually evaluates to true, retrying on any acceptable errors.
 *
 * This is a special case of {@link assert.soon}.
 *
 * @param {Function} func Function to be executed, or string to be `eval`ed.
 * @param {Error|Error[]} acceptableErrors Error (or array of Errors) that are allowed.
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {number} [timeout] Timeout in ms. In CI, this is 10min, otherwise 90sec.
 * @param {number} [interval] Interval in ms to wait between tries, default 200ms.
 * @param {{runHangAnalyzer: boolean}} [opts] Options to control hang analyzer via `runHangAnalyzer`
 *     property.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.soonRetryOnAcceptableErrors(() => {
 *     assert.commandWorked(
 *         db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}));
 *     return true;
 * }, ErrorCodes.FailedToSatisfyReadPreference);
 */
assert.soonRetryOnAcceptableErrors = function(
    func, acceptableErrors, msg, timeout, interval, {runHangAnalyzer = true} = {}, attr) {
    if (!Array.isArray(acceptableErrors)) {
        acceptableErrors = [acceptableErrors];
    }

    const funcWithRetries = () => {
        try {
            return func();
        } catch (e) {
            if (acceptableErrors.some((err) => e.code === err)) {
                print("assert.soonRetryOnAcceptableErrors() retrying on err: " + tojson(e));
                return false;
            }
            throw e;
        }
    };

    assert.soon(funcWithRetries, msg, timeout, interval, {runHangAnalyzer}, attr);
};

/**
 * Assert that a function eventually evaluates to true, retrying on Network errors.
 *
 * This is a special case of {@link assert.soonRetryOnAcceptableErrors}.
 *
 * @param {Function} func Function to be executed, or string to be `eval`ed.
 * @param {string|Function|object} [msg] Failure message, displayed when the assertion fails.
 *            If a function, it is invoked and its result is used as the failure message.
 *            If an object, its conversion to json is used as the failure message.
 * @param {number} [timeout] Timeout in ms. In CI, this is 10min, otherwise 90sec.
 * @param {number} [interval] Interval in ms to wait between tries, default 200ms.
 * @param {{runHangAnalyzer: boolean}} [opts] Options to control hang analyzer via `runHangAnalyzer`
 *     property.
 * @param {object} [attr] Additional attributes to be included in failure messages.
 *
 * @throws {Error} if assertion is not satisfied.
 *
 * @example
 * assert.soonRetryOnNetworkErrors(() => {
 *     primaryInfo = db.isMaster();
 *     return primaryInfo.hasOwnProperty("ismaster") && primaryInfo.ismaster;
 * });
 */
assert.soonRetryOnNetworkErrors = function(
    func, msg, timeout, interval, {runHangAnalyzer = true} = {}, attr) {
    let acceptableErrors = Array.from(ErrorCodes.NetworkError);
    assert.soonRetryOnAcceptableErrors(
        func, acceptableErrors, msg, timeout, interval, runHangAnalyzer, attr);
};

export {doassert, sortDoc, assert, formatErrorMsg};
