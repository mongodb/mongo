doassert = function(msg, obj) {
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

// Sort doc/obj fields and return new sorted obj
sortDoc = function(doc) {
    // Helper to sort the elements of the array
    const sortElementsOfArray = function(arr) {
        let newArr = [];
        if (!arr || arr.constructor != Array)
            return arr;
        for (let i = 0; i < arr.length; i++) {
            newArr.push(sortDoc(arr[i]));
        }
        return newArr;
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

assert = (function() {
    // Wrapping the helper function in an IIFE to avoid polluting the global namespace.

    function _processMsg(msg) {
        if (typeof msg === "function") {
            msg = msg();
        }

        if (typeof msg === "object") {
            msg = tojson(msg);
        }
        return msg;
    }

    function _doassert(msg, prefix, attr, prefixOverride) {
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
            doassert(_buildAssertionMessage(msg, prefixOverride ?? prefix), attr);
        }
        doassert(_buildAssertionMessage(msg, prefix), attr?.res);
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

    var assert = function(value, msg, attr) {
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

    assert.eq = function(a, b, msg, attr) {
        _validateAssertionMessage(msg, attr);

        if (_isEq(a, b)) {
            return;
        }

        _doassert(msg,
                  `[${tojson(a)}] != [${tojson(b)}] are not equal`,
                  {a, b, ...attr},
                  "assert.eq() failed");
    };

    function _isDocEq(a, b) {
        return a === b || bsonUnorderedFieldsCompare(a, b) === 0;
    }

    /**
     * Throws if 'actualDoc' object is not equal to 'expectedDoc' object. The order of fields
     * (properties) within objects is disregarded.
     * Throws if object representation in BSON exceeds 16793600 bytes.
     */
    assert.docEq = function(expectedDoc, actualDoc, msg, attr) {
        _validateAssertionMessage(msg, attr);

        if (_isDocEq(expectedDoc, actualDoc)) {
            return;
        }

        _doassert(msg,
                  "expected document " + tojson(expectedDoc) + " and actual document " +
                      tojson(actualDoc) + " are not equal",
                  {expectedDoc, actualDoc, ...attr},
                  "assert.docEq() failed");
    };

    /**
     * Throws if the elements of the two given sets are not the same. Use only for primitive
     * (non-object) set element types.
     */
    assert.setEq = function(expectedSet, actualSet, msg, attr) {
        _validateAssertionMessage(msg, attr);

        const failAssertion = function() {
            _doassert(
                msg,
                "expected set " + tojson(expectedSet) + " and actual set " + tojson(actualSet) +
                    " are not equal",
                {expectedSet: Array.from(expectedSet), actualSet: Array.from(actualSet), ...attr},
                "assert.setEq() failed");
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
     * Throws if the two arrays do not have the same members, in any order. By default, nested
     * arrays must have the same order to be considered equal.
     *
     * Optionally accepts a compareFn to compare values instead of using docEq.
     */
    assert.sameMembers = function(aArr, bArr, msg, compareFn = _isDocEq, attr) {
        _validateAssertionMessage(msg, attr);

        const failAssertion = function() {
            _doassert(msg,
                      tojson(aArr) + " != " + tojson(bArr),
                      {aArr, bArr, compareFn: compareFn.name, ...attr},
                      "assert.sameMembers() failed");
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

    // Given two arrays of documents, check that each array has the same members, but,
    // for the numeric fields specified in 'fuzzyFields,' the values need to be 'close,' but do
    // not have to be equal.
    assert.fuzzySameMembers = function(aArr, bArr, fuzzyFields, msg, places = 4, attr) {
        function fuzzyCompare(docA, docB) {
            return _fieldsClose(docA, docB, fuzzyFields, msg, places);
        }
        return assert.sameMembers(aArr, bArr, msg, fuzzyCompare, attr);
    };

    assert.neq = function(a, b, msg, attr) {
        _validateAssertionMessage(msg, attr);

        if (!_isEq(a, b)) {
            return;
        }

        _doassert(msg,
                  `[${tojson(a)}] == [${tojson(b)}] are equal`,
                  {a, b, ...attr},
                  "assert.neq() failed");
    };

    assert.hasFields = function(result, arr, msg, attr) {
        if (!Array.isArray(arr)) {
            _doassert("The second argument to assert.hasFields must be an array.");
        }

        let count = 0;
        for (let field in result) {
            if (arr.includes(field)) {
                count += 1;
            }
        }

        if (count != arr.length) {
            _doassert(msg,
                      "Not all of the values from " + tojson(arr) + " were in " + tojson(result),
                      {result, arr, ...attr},
                      "assert.hasFields() failed");
        }
    };

    assert.contains = function(element, arr, msg, attr) {
        if (!Array.isArray(arr)) {
            _doassert("The second argument to assert.contains() must be an array.");
        }

        for (let i = 0; i < arr.length; i++) {
            const comp = arr[i];
            const satisfied = comp == element ||
                ((comp != null && element != null) && friendlyEqual(comp, element));
            if (satisfied) {
                return;
            }
        }

        _doassert(msg,
                  tojson(element) + " was not in " + tojson(arr),
                  {element, arr, ...attr},
                  "assert.contains() failed");
    };

    assert.doesNotContain = function(element, arr, msg, attr) {
        if (!Array.isArray(arr)) {
            _doassert("The second argument to assert.doesNotContain must be an array.");
        }

        for (let i = 0; i < arr.length; i++) {
            const comp = arr[i];
            const match = comp == element ||
                ((comp != null && element != null) && friendlyEqual(comp, element));
            if (match) {
                _doassert(msg,
                          tojson(element) + " is in " + tojson(arr),
                          {element, arr, ...attr},
                          "assert.doesNotContain() failed");
            }
        }
    };

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

        _doassert(msg,
                  tojson(prefix) + " was not a prefix in " + tojson(arr),
                  {prefix, arr, ...attr},
                  "assert.containsPrefix() failed");
    };

    /*
     * Calls a function 'func' at repeated intervals until either func() returns true
     * or more than 'timeout' milliseconds have elapsed. Throws an exception with
     * message 'msg' after timing out.
     */
    assert.soon = function(
        func, msg, timeout, interval = 200, {runHangAnalyzer = true} = {}, attr) {
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
                    msg = msg +
                        " The hang analyzer is automatically called in assert.soon functions." +
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

    /*
     * Calls a function 'func' at repeated intervals until either func() returns true without
     * throwing an exception or more than 'timeout' milliseconds have elapsed. Throws an exception
     * with message 'msg' after timing out.
     */
    assert.soonNoExcept = function(
        func, msg, timeout, interval, {runHangAnalyzer = true} = {}, attr) {
        const safeFunc =
            _convertExceptionToReturnStatus(func, "assert.soonNoExcept caught exception");
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

    /*
     * Calls the given function 'func' repeatedly at time intervals specified by
     * 'intervalMS' (milliseconds) until either func() returns true or the number of
     * attempted function calls is equal to 'num_attempts'. Throws an exception with
     * message 'msg' after all attempts are used up. If no 'intervalMS' argument is passed,
     * it defaults to 0.
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
        _doassert(msg, null, attr, "assert.retry() failed");
    };

    /*
     * Calls the given function 'func' repeatedly at time intervals specified by
     * 'intervalMS' (milliseconds) until either func() returns true without throwing
     * an exception or the number of attempted function calls is equal to 'num_attempts'.
     * Throws an exception with message 'msg' after all attempts are used up. If no 'intervalMS'
     * argument is passed, it defaults to 0.
     */
    assert.retryNoExcept = function(
        func, msg, num_attempts, intervalMS, {runHangAnalyzer = true} = {}, attr) {
        const safeFunc =
            _convertExceptionToReturnStatus(func, "assert.retryNoExcept caught exception");
        assert.retry(safeFunc, msg, num_attempts, intervalMS, {runHangAnalyzer}, attr);
    };

    /**
     * Runs the given command on the 'admin' database of the provided node. Asserts that the command
     * worked but allows network errors to occur.
     *
     * Returns the response if the command succeeded, or undefined if the command failed, *even* if
     * the failure was due to a network error.
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

        const msgPrefix =
            "assert.time failed timeout " + timeout + "ms took " + diff + "ms : " + f + ", msg";
        msg = _buildAssertionMessage(msg);
        if (runHangAnalyzer) {
            msg = msg + " The hang analyzer is automatically called in assert.time functions. " +
                "If you are *expecting* assert.soon to possibly fail, call assert.time " +
                "with {runHangAnalyzer: false} as the fourth argument " +
                "(you can fill unused arguments with `undefined`).";
            print(msg + " Running hang analyzer from assert.time.");
            MongoRunner.runHangAnalyzer();
        }
        _doassert(
            msg, msgPrefix, {timeMS: diff, timeoutMS: timeout, ...attr}, "assert.time() failed");
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
                      `[${tojson(error.code)}] != [${tojson(expectedCode)}] are not equal`,
                      {code: error.code, expectedCode, ...attr},
                      "assert.throwsWithCode() failed");
        }
        return error;
    };

    assert.doesNotThrow = function(func, params, msg, attr) {
        _validateAssertionMessage(msg, attr);

        // Use .apply() instead of calling the function directly with explicit arguments to
        // preserve the length of the `arguments` object.
        const {error, res} = assertThrowsHelper.apply(null, arguments);

        if (error) {
            const {code, message} = error;
            _doassert(msg,
                      "threw unexpected exception: " + error,
                      {error: {...(code && {code}), ...(message && {message})}, ...attr},
                      "assert.doesNotThrow() failed");
        }

        return res;
    };

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
                      `expected result type 'object', got '${typeof res}', res='${res}'`,
                      {result: res, resultType: typeof res},
                      "expected result type 'object'");
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
            let prefix = "command failed: " + tojson(res);
            if (typeof res._commandObj === "object" && res._commandObj !== null) {
                prefix += " with original command request: " + tojson(res._commandObj);
            }
            if (typeof res._mongo === "object" && res._mongo !== null) {
                prefix += " on connection: " + res._mongo;
            }
            return prefix;
        };
        const makeFailPrefixOverride = (res) => {
            if (res.hasOwnProperty("errmsg")) {
                return `command failed: ${res.errmsg}`;
            }
            return "command failed";
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
            _doassert(msg, makeFailPrefix(res), {res}, makeFailPrefixOverride(res));
        } else if (res.hasOwnProperty("ok")) {
            // Handle raw command responses or cases like MapReduceResult which extend command
            // response.
            if (!_rawReplyOkAndNoWriteErrors(res, {
                    ignoreWriteErrors: ignoreWriteErrors,
                    ignoreWriteConcernErrors: ignoreWriteConcernErrors
                })) {
                _runHangAnalyzerForSpecificFailureTypes(res);
                _doassert(msg, makeFailPrefix(res), {res}, makeFailPrefixOverride(res));
            }
        } else if (res.hasOwnProperty("acknowledged")) {
            // CRUD api functions return plain js objects with an acknowledged property.
            // no-op.
        } else {
            _doassert(msg,
                      "unknown type of result, cannot check ok: " + tojson(res),
                      {res},
                      "unknown type of result");
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
            return "command worked when it should have failed: " + tojson(res);
        };
        const makeFailPrefixOverride = (res) => {
            if (res.hasOwnProperty("errmsg")) {
                return `command worked when it should have failed: ${res.errmsg}`;
            }
            return "command worked when it should have failed";
        };

        const makeFailCodePrefix = (res, expectedCode) => {
            return (expectedCode !== assert._kAnyErrorCode)
                ? "command did not fail with any of the following codes " + tojson(expectedCode) +
                    " " + tojson(res)
                : null;
        };
        const makeFailCodePrefixOverride = (res, expectedCode) => {
            if (res.hasOwnProperty("errmsg")) {
                return (expectedCode !== assert._kAnyErrorCode)
                    ? "command did not fail with any of the following codes " +
                        tojson(expectedCode) + " " + res.errmsg
                    : null;
            }
            return (expectedCode !== assert._kAnyErrorCode)
                ? "command did not fail with any of the following codes " + tojson(expectedCode)
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
                    _doassert(msg,
                              makeFailCodePrefix(res, expectedCode),
                              {res, expectedCode},
                              makeFailCodePrefixOverride(res, expectedCode));
                }
            }
        } else if (res.hasOwnProperty("ok")) {
            // Handle raw command responses or cases like MapReduceResult which extend command
            // response.
            if (_rawReplyOkAndNoWriteErrors(res)) {
                _doassert(msg,
                          makeFailPrefix(res, expectedCode),
                          {res},
                          makeFailPrefixOverride(res, expectedCode));
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
                    _doassert(msg,
                              makeFailCodePrefix(res, expectedCode),
                              {res, expectedCode},
                              makeFailCodePrefixOverride(res, expectedCode));
                }
            }
        } else if (res.hasOwnProperty("acknowledged")) {
            // CRUD api functions return plain js objects with an acknowledged property.
            _doassert(msg,
                      makeFailPrefix(res, expectedCode),
                      {res},
                      makeFailPrefixOverride(res, expectedCode));
        } else {
            _doassert(msg,
                      "unknown type of result, cannot check error: " + tojson(res),
                      {res},
                      "unknown type of result");
        }
        return res;
    }

    assert.commandWorkedOrFailedWithCode = function commandWorkedOrFailedWithCode(
        res, errorCodeSet, msg) {
        try {
            // First check if the command worked.
            return assert.commandWorked(res, msg);
        } catch (e) {
            // If the command did not work, assert it failed with one of the specified codes.
            return assert.commandFailedWithCode(res, errorCodeSet, msg);
        }
    };

    assert.commandWorkedIgnoringWriteConcernErrorsOrFailedWithCode =
        function commandWorkedOrFailedWithCode(res, errorCodeSet, msg) {
        try {
            // First check if the command worked.
            return _assertCommandWorked(res, msg, {ignoreWriteConcernErrors: true});
        } catch (e) {
            // If the command did not work, assert it failed with one of the specified codes.
            return assert.commandFailedWithCode(res, errorCodeSet, msg);
        }
    };

    assert.commandWorked = function(res, msg) {
        return _assertCommandWorked(res, msg, {ignoreWriteErrors: false});
    };

    assert.commandWorkedIgnoringWriteErrors = function(res, msg) {
        return _assertCommandWorked(res, msg, {ignoreWriteErrors: true});
    };

    assert.commandWorkedIgnoringWriteConcernErrors = function(res, msg) {
        return _assertCommandWorked(res, msg, {ignoreWriteConcernErrors: true});
    };

    assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors = function(res, msg) {
        return _assertCommandWorked(
            res, msg, {ignoreWriteConcernErrors: true, ignoreWriteErrors: true});
    };

    assert.commandFailed = function(res, msg) {
        return _assertCommandFailed(res, assert._kAnyErrorCode, msg);
    };

    // expectedCode can be an array of possible codes.
    assert.commandFailedWithCode = function(res, expectedCode, msg) {
        return _assertCommandFailed(res, expectedCode, msg);
    };

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
            _doassert(msg, errMsg + ": " + tojson(res), {res}, errMsg);
        }

        return res;
    };

    assert.writeError = function(res, msg) {
        return assert.writeErrorWithCode(res, assert._kAnyErrorCode, msg);
    };

    // If expectedCode is an array then this asserts that the found code is one of the codes in
    // the expectedCode array.
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
            _doassert(msg, errMsg + ": " + tojson(res), {res}, errMsg);
        }

        if (expectedCode !== assert._kAnyErrorCode) {
            if (!Array.isArray(expectedCode)) {
                expectedCode = [expectedCode];
            }
            const found = expectedCode.some((ec) => writeErrorCodes.has(ec));
            if (!found) {
                errMsg = "found code(s) " + tojson(Array.from(writeErrorCodes)) +
                    " does not match any of the expected codes " + tojson(expectedCode) +
                    ". Original command response: " + tojson(res);
                _runHangAnalyzerForSpecificFailureTypes(res);
                _doassert(msg,
                          errMsg,
                          {res, expectedCode, writeErrorCodes: Array.from(writeErrorCodes)},
                          "found code(s) does not match any of the expected codes");
            }
        }

        return res;
    };

    assert.isnull = function(value, msg, attr) {
        _validateAssertionMessage(msg, attr);

        if (value == null) {
            return;
        }

        _doassert(msg,
                  "supposed to be null, was: " + tojson(value),
                  {value, ...attr},
                  "assert.isnull() failed");
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

        _doassert(msg,
                  a + " is not " + description + " " + b,
                  {a, b, ...attr},
                  `assert ${description} failed`);
    }

    assert.lt = function(a, b, msg, attr) {
        _assertCompare((a, b) => {
            return a < b;
        }, a, b, "less than", msg, attr);
    };

    assert.gt = function(a, b, msg, attr) {
        _assertCompare((a, b) => {
            return a > b;
        }, a, b, "greater than", msg, attr);
    };

    assert.lte = function(a, b, msg, attr) {
        _assertCompare((a, b) => {
            return a <= b;
        }, a, b, "less than or eq", msg, attr);
    };

    assert.gte = function(a, b, msg, attr) {
        _assertCompare((a, b) => {
            return a >= b;
        }, a, b, "greater than or eq", msg, attr);
    };

    assert.between = function(a, b, c, msg, inclusive = true, attr) {
        _validateAssertionMessage(msg, attr);

        let compareFn = (a, b) => {
            return inclusive ? a <= b : a < b;
        };

        if (_compare(compareFn, a, b) && _compare(compareFn, b, c)) {
            return;
        }

        _doassert(msg,
                  b + " is not between " + a + " and " + c,
                  {a, b, c, inclusive, ...attr},
                  "assert.between() failed");
    };

    assert.betweenIn = function(a, b, c, msg, attr) {
        assert.between(a, b, c, msg, true, attr);
    };
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

    // Assert that numerical values are equivalent to 'places' significant figures.
    assert.close = function(a, b, msg, places = 4) {
        const [isClose, errMsg] = _isClose(a, b, places);
        if (!isClose) {
            _doassert(msg, errMsg, {a, b, places}, "assert.close() failed");
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
                    if (!_isClose(
                            parseFloat(docA[currField]), parseFloat(docB[currField]), places)) {
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
     * Asserts if the times in millis are not withing delta milliseconds, in either direction.
     * Default Delta: 1 second
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
        _doassert(msg,
                  msgPrefix,
                  {a: forLog(a), b: forLog(b), deltaMS, ...attr},
                  "assert.closeWithinMS() failed");
    };

    assert.includes = function(haystack, needle, msg, attr) {
        if (haystack.includes(needle)) {
            return;
        }

        const prefix = `string [${haystack}] does not include [${needle}]`;
        _doassert(msg, prefix, {haystack, needle, ...attr}, "assert.includes() failed");
    };

    assert.noAPIParams = function(cmdOptions) {
        if (!(cmdOptions instanceof Object)) {
            return;
        }
        assert(!cmdOptions.hasOwnProperty("apiVersion") &&
                   !cmdOptions.hasOwnProperty("apiStrict") &&
                   !cmdOptions.hasOwnProperty("apiDeprecationErrors"),
               "API parameters are not allowed in this context");
    };

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

    /*
     * Calls a function 'func' at repeated intervals of 'interval' milliseconds until either func()
     * returns true or more than 'timeout' milliseconds have elapsed. Throws an exception with
     * message 'msg' after timing out.
     *
     * If 'func' encounters a NetworkError, the exception will be ignored, and 'func' will be called
     * again.
     */
    assert.soonRetryOnNetworkErrors = function(
        func, msg, timeout, interval, {runHangAnalyzer = true} = {}, attr) {
        let acceptableErrors = Array.from(ErrorCodes.NetworkError);
        assert.soonRetryOnAcceptableErrors(
            func, acceptableErrors, msg, timeout, interval, runHangAnalyzer, attr);
    };

    return assert;
})();
