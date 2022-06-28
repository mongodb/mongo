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

    var ex;
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
    var sortElementsOfArray = function(arr) {
        var newArr = [];
        if (!arr || arr.constructor != Array)
            return arr;
        for (var i = 0; i < arr.length; i++) {
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

    var newDoc = {};
    var fields = Object.keys(doc);
    if (fields.length > 0) {
        fields.sort();
        for (var i = 0; i < fields.length; i++) {
            var field = fields[i];
            if (doc.hasOwnProperty(field)) {
                var tmp = doc[field];

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
    } else {
        newDoc = doc;
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
    var safeFunc = () => {
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
            return msg();
        } else if (typeof msg === "object") {
            return tojson(msg);
        }

        return msg;
    }

    function _validateAssertionMessage(msg) {
        if (msg) {
            if (typeof msg === "function") {
                if (msg.length !== 0) {
                    doassert("msg function cannot expect any parameters.");
                }
            } else if (typeof msg !== "string" && typeof msg !== "object") {
                doassert("msg parameter must be a string, function or object. Found type: " +
                         typeof (msg));
            }

            if (msg && assert._debug) {
                print("in assert for: " + _processMsg(msg));
            }
        }
    }

    function _buildAssertionMessage(msg, prefix) {
        var fullMessage = '';

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

    var assert = function(b, msg) {
        if (arguments.length > 2) {
            doassert("Too many parameters to assert().");
        }

        _validateAssertionMessage(msg);

        if (b) {
            return;
        }

        doassert(_buildAssertionMessage(msg, "assert failed"));
    };

    assert.automsg = function(b) {
        assert(eval(b), b);
    };

    assert._debug = false;

    function _isEq(a, b) {
        if (a == b) {
            return true;
        }

        if ((a != null && b != null) && friendlyEqual(a, b)) {
            return true;
        }

        return false;
    }

    assert.eq = function(a, b, msg) {
        _validateAssertionMessage(msg);

        if (_isEq(a, b)) {
            return;
        }

        doassert(_buildAssertionMessage(
            msg, "[" + tojson(a) + "] != [" + tojson(b) + "] are not equal"));
    };

    function _isDocEq(a, b) {
        if (a == b) {
            return true;
        }

        var aSorted = sortDoc(a);
        var bSorted = sortDoc(b);

        if ((aSorted != null && bSorted != null) && friendlyEqual(aSorted, bSorted)) {
            return true;
        }

        return false;
    }

    assert.docEq = function(a, b, msg) {
        _validateAssertionMessage(msg);

        if (_isDocEq(a, b)) {
            return;
        }

        doassert(_buildAssertionMessage(
            msg, "[" + tojson(a) + "] != [" + tojson(b) + "] are not equal"));
    };

    assert.setEq = function(aSet, bSet, msg) {
        const failAssertion = function() {
            doassert(_buildAssertionMessage(msg, tojson(aSet) + " != " + tojson(bSet)));
        };
        if (aSet.size !== bSet.size) {
            failAssertion();
        }
        for (let a of aSet) {
            if (!bSet.has(a)) {
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
    assert.sameMembers = function(aArr, bArr, msg, compareFn = _isDocEq) {
        _validateAssertionMessage(msg);

        const failAssertion = function() {
            doassert(_buildAssertionMessage(msg, tojson(aArr) + " != " + tojson(bArr)));
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

    assert.eq.automsg = function(a, b) {
        assert.eq(eval(a), eval(b), "[" + a + "] != [" + b + "]");
    };

    assert.neq = function(a, b, msg) {
        _validateAssertionMessage(msg);

        if (!_isEq(a, b)) {
            return;
        }

        doassert(_buildAssertionMessage(msg, "[" + a + "] != [" + b + "] are equal"));
    };

    assert.hasFields = function(result, arr, msg) {
        var count = 0;
        if (!Array.isArray(arr)) {
            throw new Error("The second argument to assert.hasFields must be an array.");
        }

        for (var field in result) {
            if (arr.includes(field)) {
                count += 1;
            }
        }

        if (count != arr.length) {
            doassert(_buildAssertionMessage(
                msg, "Not all of the values from " + tojson(arr) + " were in " + tojson(result)));
        }
    };

    assert.contains = function(o, arr, msg) {
        var wasIn = false;
        if (!Array.isArray(arr)) {
            throw new Error("The second argument to assert.contains must be an array.");
        }

        for (var i = 0; i < arr.length; i++) {
            wasIn = arr[i] == o || ((arr[i] != null && o != null) && friendlyEqual(arr[i], o));
            if (wasIn) {
                break;
            }
        }

        if (!wasIn) {
            doassert(_buildAssertionMessage(msg, tojson(o) + " was not in " + tojson(arr)));
        }
    };

    assert.containsPrefix = function(prefix, arr, msg) {
        var wasIn = false;
        if (typeof (prefix) !== "string") {
            throw new Error("The first argument to containsPrefix must be a string.");
        }
        if (!Array.isArray(arr)) {
            throw new Error("The second argument to containsPrefix must be an array.");
        }

        for (let i = 0; i < arr.length; i++) {
            if (typeof (arr[i]) !== "string") {
                continue;
            }

            wasIn = arr[i].startsWith(prefix);
            if (wasIn) {
                break;
            }
        }

        if (!wasIn) {
            doassert(_buildAssertionMessage(
                msg, tojson(prefix) + " was not a prefix in " + tojson(arr)));
        }
    };

    /*
     * Calls a function 'func' at repeated intervals until either func() returns true
     * or more than 'timeout' milliseconds have elapsed. Throws an exception with
     * message 'msg' after timing out.
     */
    assert.soon = function(func, msg, timeout, interval, {runHangAnalyzer = true} = {}) {
        _validateAssertionMessage(msg);

        var msgPrefix = "assert.soon failed: " + func;

        if (msg) {
            if (typeof (msg) != "function") {
                msgPrefix = "assert.soon failed, msg";
            }
        }

        var start = new Date();

        if (TestData && TestData.inEvergreen) {
            timeout = timeout || 10 * 60 * 1000;
        } else {
            timeout = timeout || 90 * 1000;
        }

        interval = interval || 200;

        while (1) {
            if (typeof (func) == "string") {
                if (eval(func))
                    return;
            } else {
                if (func())
                    return;
            }

            diff = (new Date()).getTime() - start.getTime();
            if (diff > timeout) {
                msg = _buildAssertionMessage(msg, msgPrefix);
                if (runHangAnalyzer) {
                    msg = msg +
                        " The hang analyzer is automatically called in assert.soon functions." +
                        " If you are *expecting* assert.soon to possibly fail, call assert.soon" +
                        " with {runHangAnalyzer: false} as the fifth argument" +
                        " (you can fill unused arguments with `undefined`).";
                    print(msg + " Running hang analyzer from assert.soon.");
                    MongoRunner.runHangAnalyzer();
                }
                doassert(msg);
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
    assert.soonNoExcept = function(func, msg, timeout, interval) {
        var safeFunc =
            _convertExceptionToReturnStatus(func, "assert.soonNoExcept caught exception");
        var getFunc = () => {
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

        assert.soon(getFunc(), msg, timeout, interval);
    };

    /*
     * Calls the given function 'func' repeatedly at time intervals specified by
     * 'intervalMS' (milliseconds) until either func() returns true or the number of
     * attempted function calls is equal to 'num_attempts'. Throws an exception with
     * message 'msg' after all attempts are used up. If no 'intervalMS' argument is passed,
     * it defaults to 0.
     */
    assert.retry = function(func, msg, num_attempts, intervalMS, {runHangAnalyzer = true} = {}) {
        var intervalMS = intervalMS || 0;
        var attempts_made = 0;
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
        doassert(msg);
    };

    /*
     * Calls the given function 'func' repeatedly at time intervals specified by
     * 'intervalMS' (milliseconds) until either func() returns true without throwing
     * an exception or the number of attempted function calls is equal to 'num_attempts'.
     * Throws an exception with message 'msg' after all attempts are used up. If no 'intervalMS'
     * argument is passed, it defaults to 0.
     */
    assert.retryNoExcept = function(func, msg, num_attempts, intervalMS) {
        var safeFunc =
            _convertExceptionToReturnStatus(func, "assert.retryNoExcept caught exception");
        assert.retry(safeFunc, msg, num_attempts, intervalMS);
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

    assert.time = function(f, msg, timeout /*ms*/, {runHangAnalyzer = true} = {}) {
        _validateAssertionMessage(msg);

        var start = new Date();
        timeout = timeout || 30000;
        if (typeof (f) == "string") {
            res = eval(f);
        } else {
            res = f();
        }

        diff = (new Date()).getTime() - start.getTime();
        if (diff > timeout) {
            const msgPrefix =
                "assert.time failed timeout " + timeout + "ms took " + diff + "ms : " + f + ", msg";
            msg = _buildAssertionMessage(msg, msgPrefix);
            if (runHangAnalyzer) {
                msg = msg +
                    " The hang analyzer is automatically called in assert.time functions. " +
                    "If you are *expecting* assert.soon to possibly fail, call assert.time " +
                    "with {runHangAnalyzer: false} as the fourth argument " +
                    "(you can fill unused arguments with `undefined`).";
                print(msg + " Running hang analyzer from assert.time.");
                MongoRunner.runHangAnalyzer();
            }
            doassert(msg);
        }
        return res;
    };

    function assertThrowsHelper(func, params) {
        if (typeof func !== "function") {
            throw new Error('1st argument must be a function');
        }

        if (arguments.length >= 2 && !Array.isArray(params) &&
            Object.prototype.toString.call(params) !== "[object Arguments]") {
            throw new Error("2nd argument must be an Array or Arguments object");
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
            doassert("Attempted to access 'this' during function call in" +
                     " assert.throws/doesNotThrow. Instead, wrap the function argument in" +
                     " another function.");
        }

        return {error: error, res: res};
    }

    assert.throws = function(func, params, msg) {
        _validateAssertionMessage(msg);

        // Use .apply() instead of calling the function directly with explicit arguments to
        // preserve the length of the `arguments` object.
        const {error} = assertThrowsHelper.apply(null, arguments);

        if (!error) {
            doassert(_buildAssertionMessage(msg, "did not throw exception"));
        }

        return error;
    };

    assert.throwsWithCode = function(func, expectedCode, params, msg) {
        if (arguments.length < 2) {
            throw new Error("assert.throwsWithCode expects at least 2 arguments");
        }
        // Remove the 'code' parameter, and any undefined parameters, from the list of arguments.
        // Use .apply() to preserve the length of the 'arguments' object.
        const newArgs = [func, params, msg].filter(element => element !== undefined);
        const error = assert.throws.apply(null, newArgs);
        if (!Array.isArray(expectedCode)) {
            expectedCode = [expectedCode];
        }
        if (!expectedCode.some((ec) => error.code == ec)) {
            doassert(_buildAssertionMessage(
                msg,
                "[" + tojson(error.code) + "] != [" + tojson(expectedCode) + "] are not equal"));
        }
        return error;
    };

    assert.doesNotThrow = function(func, params, msg) {
        _validateAssertionMessage(msg);

        // Use .apply() instead of calling the function directly with explicit arguments to
        // preserve the length of the `arguments` object.
        const {error, res} = assertThrowsHelper.apply(null, arguments);

        if (error) {
            doassert(_buildAssertionMessage(msg, "threw unexpected exception: " + error));
        }

        return res;
    };

    assert.dropExceptionsWithCode = function(func, dropCodes, onDrop) {
        if (typeof func !== "function") {
            doassert('assert.dropExceptionsWithCode 1st argument must be a function');
        }
        if (typeof onDrop !== "function") {
            doassert('assert.dropExceptionsWithCode 3rd argument must be a function');
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

    assert.throws.automsg = function(func, params) {
        if (arguments.length === 1)
            params = [];
        assert.throws(func, params, func.toString());
    };

    assert.doesNotThrow.automsg = function(func, params) {
        if (arguments.length === 1)
            params = [];
        assert.doesNotThrow(func, params, func.toString());
    };

    function _rawReplyOkAndNoWriteErrors(raw, {ignoreWriteErrors, ignoreWriteConcernErrors} = {}) {
        if (raw.ok === 0) {
            return false;
        }

        // A write command response may have ok:1 but write errors.
        if (!ignoreWriteErrors && raw.hasOwnProperty("writeErrors") && raw.writeErrors.length > 0) {
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
            doassert("unknown response type '" + typeof res + "' given to " + assertionName +
                     ", res='" + res + "'");
        }
    }

    function _runHangAnalyzerIfWriteConcernTimedOut(res) {
        const timeoutMsg = "waiting for replication timed out";
        let isWriteConcernTimeout = false;
        if (_isWriteResultType(res)) {
            if (res.hasWriteConcernError() && res.getWriteConcernError().errmsg === timeoutMsg) {
                isWriteConcernTimeout = true;
            }
        } else if ((res.hasOwnProperty("errmsg") && res.errmsg === timeoutMsg) ||
                   (res.hasOwnProperty("writeConcernError") &&
                    res.writeConcernError.errmsg === timeoutMsg)) {
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
        const isTransientTxnError = res.hasOwnProperty("errorLabels") &&
            res.errorLabels.includes("TransientTransactionError");
        const isLockTimeout = res.hasOwnProperty("code") && ErrorCodes.LockTimeout === res.code;
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
        const makeFailMsg = () => {
            let prefix = "command failed: " + tojson(res);
            if (typeof res._commandObj === "object" && res._commandObj !== null) {
                prefix += " with original command request: " + tojson(res._commandObj);
            }
            if (typeof res._mongo === "object" && res._mongo !== null) {
                prefix += " on connection: " + res._mongo;
            }
            return _buildAssertionMessage(msg, prefix);
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
            doassert(makeFailMsg(), res);
        } else if (res.hasOwnProperty("ok")) {
            // Handle raw command responses or cases like MapReduceResult which extend command
            // response.
            if (!_rawReplyOkAndNoWriteErrors(res, {
                    ignoreWriteErrors: ignoreWriteErrors,
                    ignoreWriteConcernErrors: ignoreWriteConcernErrors
                })) {
                _runHangAnalyzerForSpecificFailureTypes(res);
                doassert(makeFailMsg(), res);
            }
        } else if (res.hasOwnProperty("acknowledged")) {
            // CRUD api functions return plain js objects with an acknowledged property.
            // no-op.
        } else {
            doassert(_buildAssertionMessage(
                         msg, "unknown type of result, cannot check ok: " + tojson(res)),
                     res);
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
        const makeFailMsg = () => {
            return _buildAssertionMessage(
                msg, "command worked when it should have failed: " + tojson(res));
        };

        const makeFailCodeMsg = () => {
            return (expectedCode !== assert._kAnyErrorCode)
                ? _buildAssertionMessage(msg,
                                         "command did not fail with any of the following codes " +
                                             tojson(expectedCode) + " " + tojson(res))
                : "";
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
                    doassert(makeFailCodeMsg(), res);
                }
            }
        } else if (res.hasOwnProperty("ok")) {
            // Handle raw command responses or cases like MapReduceResult which extend command
            // response.
            if (_rawReplyOkAndNoWriteErrors(res)) {
                doassert(makeFailMsg(), res);
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
                    doassert(makeFailCodeMsg(), res);
                }
            }
        } else if (res.hasOwnProperty("acknowledged")) {
            // CRUD api functions return plain js objects with an acknowledged property.
            doassert(makeFailMsg());
        } else {
            doassert(_buildAssertionMessage(
                         msg, "unknown type of result, cannot check error: " + tojson(res)),
                     res);
        }
        return res;
    }

    assert.commandWorkedOrFailedWithCode = function commandWorkedOrFailedWithCode(
        res, errorCodeSet, msg) {
        if (!res.ok) {
            return assert.commandFailedWithCode(res, errorCodeSet, msg);
        } else {
            return assert.commandWorked(res, msg);
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
        var errMsg = null;

        if (res instanceof WriteResult) {
            if (res.hasWriteError()) {
                errMsg = "write failed with error: " + tojson(res);
            } else if (!ignoreWriteConcernErrors && res.hasWriteConcernError()) {
                errMsg = "write concern failed with errors: " + tojson(res);
            }
        } else if (res instanceof BulkWriteResult) {
            // Can only happen with bulk inserts
            if (res.hasWriteErrors()) {
                errMsg = "write failed with errors: " + tojson(res);
            } else if (!ignoreWriteConcernErrors && res.hasWriteConcernError()) {
                errMsg = "write concern failed with errors: " + tojson(res);
            }
        } else if (res instanceof WriteCommandError || res instanceof WriteError ||
                   res instanceof BulkWriteError) {
            errMsg = "write command failed: " + tojson(res);
        } else {
            if (!res || !res.ok) {
                errMsg = "unknown type of write result, cannot check ok: " + tojson(res);
            }
        }

        if (errMsg) {
            _runHangAnalyzerForSpecificFailureTypes(res);
            doassert(_buildAssertionMessage(msg, errMsg), res);
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
            doassert("assert.writeErrorWithCode called with undefined error code");
        }

        var errMsg = null;
        var writeErrorCodes = new Set();

        if (res instanceof WriteResult) {
            if (res.hasWriteError()) {
                writeErrorCodes.add(res.getWriteError().code);
            } else if (res.hasWriteConcernError()) {
                writeErrorCodes.add(res.getWriteConcernError().code);
            } else {
                errMsg = "no write error: " + tojson(res);
            }
        } else if (res instanceof BulkWriteResult || res instanceof BulkWriteError) {
            // Can only happen with bulk inserts
            if (res.hasWriteErrors()) {
                // Save every write error code.
                res.getWriteErrors().forEach((we) => writeErrorCodes.add(we.code));
            } else if (res.hasWriteConcernError()) {
                writeErrorCodes.add(res.getWriteConcernError().code);
            } else {
                errMsg = "no write errors: " + tojson(res);
            }
        } else if (res instanceof WriteCommandError) {
            // Can only happen with bulk inserts
            // No-op since we're expecting an error
        } else if (res instanceof WriteError) {
            writeErrorCodes.add(res.code);
        } else {
            if (!res || res.ok) {
                errMsg = "unknown type of write result, cannot check error: " + tojson(res);
            }
        }

        if (!errMsg && expectedCode !== assert._kAnyErrorCode) {
            if (!Array.isArray(expectedCode)) {
                expectedCode = [expectedCode];
            }
            const found = expectedCode.some((ec) => writeErrorCodes.has(ec));
            if (!found) {
                errMsg = "found code(s) " + tojson(Array.from(writeErrorCodes)) +
                    " does not match any of the expected codes " + tojson(expectedCode);
            }
        }

        if (errMsg) {
            _runHangAnalyzerForSpecificFailureTypes(res);
            doassert(_buildAssertionMessage(msg, errMsg));
        }

        return res;
    };

    assert.isnull = function(what, msg) {
        _validateAssertionMessage(msg);

        if (what == null) {
            return;
        }

        doassert("supposed to be null (" + (_processMsg(msg) || "") + ") was: " + tojson(what));
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

    function _assertCompare(f, a, b, description, msg) {
        _validateAssertionMessage(msg);

        if (_compare(f, a, b)) {
            return;
        }

        doassert(_buildAssertionMessage(msg, a + " is not " + description + " " + b));
    }

    assert.lt = function(a, b, msg) {
        _assertCompare((a, b) => {
            return a < b;
        }, a, b, "less than", msg);
    };

    assert.gt = function(a, b, msg) {
        _assertCompare((a, b) => {
            return a > b;
        }, a, b, "greater than", msg);
    };

    assert.lte = function(a, b, msg) {
        _assertCompare((a, b) => {
            return a <= b;
        }, a, b, "less than or eq", msg);
    };

    assert.gte = function(a, b, msg) {
        _assertCompare((a, b) => {
            return a >= b;
        }, a, b, "greater than or eq", msg);
    };

    assert.between = function(a, b, c, msg, inclusive) {
        _validateAssertionMessage(msg);

        let compareFn = (a, b) => {
            return a < b;
        };

        if ((inclusive == undefined || inclusive == true)) {
            compareFn = (a, b) => {
                return a <= b;
            };
        }

        if (_compare(compareFn, a, b) && _compare(compareFn, b, c)) {
            return;
        }

        doassert(_buildAssertionMessage(msg, b + " is not between " + a + " and " + c));
    };

    assert.betweenIn = function(a, b, c, msg) {
        assert.between(a, b, c, msg, true);
    };
    assert.betweenEx = function(a, b, c, msg) {
        assert.between(a, b, c, msg, false);
    };

    assert.close = function(a, b, msg, places = 4) {
        // This treats 'places' as digits past the decimal point.
        var absoluteError = Math.abs(a - b);
        if (Math.round(absoluteError * Math.pow(10, places)) === 0) {
            return;
        }

        // This treats 'places' as significant figures.
        var relativeError = Math.abs(absoluteError / b);
        if (Math.round(relativeError * Math.pow(10, places)) === 0) {
            return;
        }

        const msgPrefix = `${a} is not equal to ${b} within ${places} places, absolute error: ` +
            `${absoluteError}, relative error: ${relativeError}`;
        doassert(_buildAssertionMessage(msg, msgPrefix));
    };

    /**
     * Asserts if the times in millis are not withing delta milliseconds, in either direction.
     * Default Delta: 1 second
     */
    assert.closeWithinMS = function(a, b, msg, deltaMS) {
        "use strict";

        if (deltaMS === undefined) {
            deltaMS = 1000;
        }
        var aMS = a instanceof Date ? a.getTime() : a;
        var bMS = b instanceof Date ? b.getTime() : b;
        var actualDelta = Math.abs(Math.abs(aMS) - Math.abs(bMS));

        if (actualDelta <= deltaMS) {
            return;
        }

        const msgPrefix = "" + a + " is not equal to " + b + " within " + deltaMS + " millis, " +
            "actual delta: " + actualDelta + " millis";
        doassert(_buildAssertionMessage(msg, msgPrefix));
    };

    assert.includes = function(haystack, needle, msg) {
        if (!haystack.includes(needle)) {
            var assertMsg = "string [" + haystack + "] does not include [" + needle + "]";
            if (msg) {
                assertMsg += ", " + msg;
            }

            doassert(assertMsg);
        }
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

    return assert;
})();
