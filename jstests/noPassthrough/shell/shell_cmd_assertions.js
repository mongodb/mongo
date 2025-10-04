/**
 * Tests for the command assertion functions in mongo/shell/assert.js.
 *  @tags: [
 *   requires_scripting,
 * ]
 */

const conn = MongoRunner.runMongod();
const db = conn.getDB("commandAssertions");
const kFakeErrCode = 1234567890;
const tests = [];

const sampleWriteConcernError = {
    n: 1,
    ok: 1,
    writeConcernError: {
        code: ErrorCodes.WriteConcernTimeout,
        codeName: "WriteConcernTimeout",
        errmsg: "waiting for replication timed out",
        errInfo: {
            wtimeout: true,
        },
    },
};

function setup() {
    db.coll.drop();
    assert.commandWorked(db.coll.insert({_id: 1}));
}

// Raw command responses.
tests.push(function rawCommandOk() {
    const res = db.runCommand({"ping": 1});
    assert.doesNotThrow(() => assert.commandWorked(res));
    assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.throws(() => assert.commandFailed(res));
    assert.throws(() => assert.commandFailedWithCode(res, 0));
});

function _assertMsgFunctionExecution(assertFunc, assertParameter, {expectException: expectException = false} = {}) {
    let msgFunctionCalled = false;
    let expectedAssert = assert.doesNotThrow;

    if (expectException) {
        expectedAssert = assert.throws;
    }

    expectedAssert(() => {
        assertFunc(assertParameter, () => {
            msgFunctionCalled = true;
        });
    });

    assert.eq(expectException, msgFunctionCalled, "msg function execution should match assertion");
}

tests.push(function msgFunctionOnlyCalledOnFailure() {
    const res = db.runCommand({"ping": 1});

    _assertMsgFunctionExecution(assert.commandWorked, res, {expectException: false});
    _assertMsgFunctionExecution(assert.commandWorkedIgnoringWriteErrors, res, {expectException: false});
    _assertMsgFunctionExecution(assert.commandFailed, res, {expectException: true});

    let msgFunctionCalled = false;
    assert.throws(() =>
        assert.commandFailedWithCode(res, 0, () => {
            msgFunctionCalled = true;
        }),
    );
    assert.eq(true, msgFunctionCalled, "msg function execution should match assertion");
});

tests.push(function rawCommandErr() {
    const res = db.runCommand({"IHopeNobodyEverMakesThisACommand": 1});
    assert.throws(() => assert.commandWorked(res));
    assert.throws(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.doesNotThrow(() => assert.commandFailed(res));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.CommandNotFound));
    // commandFailedWithCode should succeed if any of the passed error codes are matched.
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, [ErrorCodes.CommandNotFound, kFakeErrCode]));
    assert.doesNotThrow(() =>
        assert.commandWorkedOrFailedWithCode(
            res,
            [ErrorCodes.CommandNotFound, kFakeErrCode],
            "threw even though failed with correct error codes",
        ),
    );
    assert.throws(() =>
        assert.commandWorkedOrFailedWithCode(
            res,
            [kFakeErrCode],
            "didn't throw even though failed with incorrect error code",
        ),
    );
});

tests.push(function rawCommandWriteOk() {
    const res = db.runCommand({insert: "coll", documents: [{_id: 2}]});
    assert.doesNotThrow(() => assert.commandWorked(res));
    assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.throws(() => assert.commandFailed(res));
    assert.throws(() => assert.commandFailedWithCode(res, 0));
    assert.doesNotThrow(() => assert.commandWorkedOrFailedWithCode(res, 0));
});

tests.push(function rawCommandWriteErr() {
    const res = db.runCommand({insert: "coll", documents: [{_id: 1}]});
    assert.throws(() => assert.commandWorked(res));
    assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.doesNotThrow(() => assert.commandFailed(res));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode]));
    assert.doesNotThrow(() =>
        assert.commandWorkedOrFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode], "expected a write failure"),
    );
    assert.throws(() => assert.commandWorkedOrFailedWithCode(res, [kFakeErrCode], "expected to throw on write error"));
});

tests.push(function collInsertWriteOk() {
    const res = db.coll.insert({_id: 2});
    assert(res instanceof WriteResult);
    assert.doesNotThrow(() => assert.commandWorked(res));
    assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.throws(() => assert.commandFailed(res));
    assert.throws(() => assert.commandFailedWithCode(res, 0));
});

tests.push(function collInsertWriteErr() {
    const res = db.coll.insert({_id: 1});
    assert(res instanceof WriteResult);
    assert.throws(() => assert.commandWorked(res));
    assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.doesNotThrow(() => assert.commandFailed(res));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode]));
});

tests.push(function collMultiInsertWriteOk() {
    const res = db.coll.insert([{_id: 3}, {_id: 2}]);
    assert(res instanceof BulkWriteResult);
    assert.doesNotThrow(() => assert.commandWorked(res));
    assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.throws(() => assert.commandFailed(res));
    assert.throws(() => assert.commandFailedWithCode(res, 0));
    assert.doesNotThrow(() => assert.commandWorkedOrFailedWithCode(res, 0, "threw even though succeeded"));
});

tests.push(function collMultiInsertWriteErr() {
    const res = db.coll.insert([{_id: 1}, {_id: 2}]);
    assert(res instanceof BulkWriteResult);
    assert.throws(() => assert.commandWorked(res));
    assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.doesNotThrow(() => assert.commandFailed(res));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode]));
});

// Test when the insert command fails with ok:0 (i.e. not failing due to write err)
tests.push(function collInsertCmdErr() {
    const res = db.coll.insert({x: 1}, {writeConcern: {"bad": 1}});
    assert(res instanceof WriteCommandError);
    assert.throws(() => assert.commandWorked(res));
    assert.throws(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.doesNotThrow(() => assert.commandFailed(res));
});

tests.push(function collMultiInsertCmdErr() {
    const res = db.coll.insert([{x: 1}, {x: 2}], {writeConcern: {"bad": 1}});
    assert(res instanceof WriteCommandError);
    assert.throws(() => assert.commandWorked(res));
    assert.throws(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.doesNotThrow(() => assert.commandFailed(res));
});

tests.push(function mapReduceOk() {
    const res = db.coll.mapReduce(
        function () {
            emit(this._id, 0);
        },
        function (k, v) {
            return v[0];
        },
        {out: "coll_out"},
    );
    assert.doesNotThrow(() => assert.commandWorked(res));
    assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.throws(() => assert.commandFailed(res));
    assert.throws(() => assert.commandFailedWithCode(res, 0));
});

tests.push(function crudInsertOneOk() {
    const res = db.coll.insertOne({_id: 2});
    assert(res.hasOwnProperty("acknowledged"));
    assert.doesNotThrow(() => assert.commandWorked(res));
    assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.throws(() => assert.commandFailed(res));
    assert.throws(() => assert.commandFailedWithCode(res, 0));
});

tests.push(function crudInsertOneErr() {
    let threw = false;
    let res = null;
    try {
        db.coll.insertOne({_id: 1});
    } catch (e) {
        threw = true;
        res = e;
    }
    assert(threw);
    assert(res instanceof WriteError);
    assert.throws(() => assert.commandWorked(res));
    assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.doesNotThrow(() => assert.commandFailed(res));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode]));
});

tests.push(function crudInsertManyOk() {
    const res = db.coll.insertMany([{_id: 2}, {_id: 3}]);
    assert(res.hasOwnProperty("acknowledged"));
    assert.doesNotThrow(() => assert.commandWorked(res));
    assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.throws(() => assert.commandFailed(res));
    assert.throws(() => assert.commandFailedWithCode(res, 0));
});

tests.push(function crudInsertManyErr() {
    let threw = false;
    let res = null;
    try {
        db.coll.insertMany([{_id: 1}, {_id: 2}]);
    } catch (e) {
        threw = true;
        res = e;
    }
    assert(threw);
    assert(res instanceof BulkWriteError);
    assert.throws(() => assert.commandWorked(res));
    assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.doesNotThrow(() => assert.commandFailed(res));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode]));
});

tests.push(function rawMultiWriteErr() {
    // Do an unordered bulk insert with duplicate keys to produce multiple write errors.
    const res = db.runCommand({"insert": "coll", documents: [{_id: 1}, {_id: 1}], ordered: false});
    assert(res.writeErrors.length == 2, "did not get multiple write errors");
    assert.throws(() => assert.commandWorked(res));
    assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.doesNotThrow(() => assert.commandFailed(res));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode]));
});

tests.push(function bulkMultiWriteErr() {
    // Do an unordered bulk insert with duplicate keys to produce multiple write errors.
    const res = db.coll.insert([{_id: 1}, {_id: 1}], {ordered: false});
    assert.throws(() => assert.commandWorked(res));
    assert.doesNotThrow(() => assert.commandWorkedIgnoringWriteErrors(res));
    assert.doesNotThrow(() => assert.commandFailed(res));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, ErrorCodes.DuplicateKey));
    assert.doesNotThrow(() => assert.commandFailedWithCode(res, [ErrorCodes.DuplicateKey, kFakeErrCode]));
});

tests.push(function writeConcernErrorCausesCommandWorkedToAssert() {
    const result = sampleWriteConcernError;

    assert.throws(() => {
        assert.commandWorked(result);
    });
});

tests.push(function writeConcernErrorCausesCommandFailedToPass() {
    const result = sampleWriteConcernError;

    assert.doesNotThrow(() => {
        assert.commandFailed(result);
        assert.commandFailedWithCode(result, ErrorCodes.WriteConcernTimeout);
    });
});

tests.push(function writeConcernErrorCanBeIgnored() {
    const result = sampleWriteConcernError;

    assert.doesNotThrow(() => {
        assert.commandWorkedIgnoringWriteConcernErrors(result);
    });
});

tests.push(function invalidResponsesAttemptToProvideInformationToCommandWorks() {
    const invalidResponses = [undefined, "not a valid response", 42];

    invalidResponses.forEach((invalidRes) => {
        const error = assert.throws(() => {
            assert.commandWorked(invalidRes);
        });
        const errorMsg = formatErrorMsg(error.message, error.extraAttr);
        assert.gte(errorMsg.indexOf(invalidRes), 0);
        assert.gte(errorMsg.indexOf(typeof invalidRes), 0);
    });
});

tests.push(function invalidResponsesAttemptToProvideInformationCommandFailed() {
    const invalidResponses = [undefined, "not a valid response", 42];

    invalidResponses.forEach((invalidRes) => {
        const error = assert.throws(() => {
            assert.commandFailed(invalidRes);
        });
        const errorMsg = formatErrorMsg(error.message, error.extraAttr);
        assert.gte(errorMsg.indexOf(invalidRes), 0);
        assert.gte(errorMsg.indexOf(typeof invalidRes), 0);
    });
});

tests.push(function assertsPreserveErrorCode() {
    function runTestOnErrorResult(res, errorCode, test) {
        let caught = false;
        try {
            test();
        } catch (e) {
            caught = true;
            assert(
                hasErrorCode(e, [errorCode]),
                "expect error code " + errorCode + " to be present in error " + tojson(e),
            );
        }
        assert(caught);
    }

    function testErrorResult(res, errorCode) {
        runTestOnErrorResult(res, errorCode, () => assert.commandWorked(res));
        runTestOnErrorResult(res, errorCode, () => assert.commandFailedWithCode(res, [errorCode + 1]));
        runTestOnErrorResult(res, errorCode, () => assert.commandWorkedOrFailedWithCode(res, [errorCode + 1]));
    }

    {
        const commandNotFound = db.runCommand({"IHopeNobodyEverMakesThisACommand": 1});
        testErrorResult(commandNotFound, ErrorCodes.CommandNotFound);
    }

    {
        const rawCommandError = db.runCommand({insert: "coll", documents: [{_id: 1}]});
        testErrorResult(rawCommandError, ErrorCodes.DuplicateKey);
    }
    {
        const insertResultError = db.coll.insert({_id: 1});
        testErrorResult(insertResultError, ErrorCodes.DuplicateKey);
    }
    {
        const insertCommandError = db.coll.insert({x: 1}, {writeConcern: {"bad": 1}});
        testErrorResult(insertCommandError, ErrorCodes.IDLUnknownField);
    }
    {
        const bulkInsertError = db.coll.insert([{_id: 1}, {_id: 2}]);
        testErrorResult(bulkInsertError, ErrorCodes.DuplicateKey);
    }
});

tests.push(function assertCallsHangAnalyzer() {
    function runAssertTest(f, expectCall) {
        const oldMongoRunner = MongoRunner;
        let runs = 0;
        try {
            MongoRunner.runHangAnalyzer = function () {
                ++runs;
            };
            f();
            assert(false);
        } catch (e) {
            if (expectCall) {
                assert.eq(runs, 1);
            } else {
                assert.eq(runs, 0);
            }
        } finally {
            MongoRunner = oldMongoRunner;
        }
    }
    const nonTimeOutWriteConcernError = {
        n: 1,
        ok: 1,
        writeConcernError: {
            code: ErrorCodes.WriteConcernTimeout,
            codeName: "WriteConcernTimeout",
            errmsg: "foo",
        },
    };

    const lockTimeoutError = {
        ok: 0,
        errmsg: "Unable to acquire lock",
        code: ErrorCodes.LockTimeout,
        codeName: "LockTimeout",
    };

    const lockTimeoutTransientTransactionError = {
        errorLabels: ["TransientTransactionError"],
        ok: 0,
        errmsg: "Unable to acquire lock",
        code: ErrorCodes.LockTimeout,
        codeName: "LockTimeout",
    };

    runAssertTest(() => assert.commandWorked(sampleWriteConcernError), true);
    runAssertTest(() => assert.commandWorked(nonTimeOutWriteConcernError), false);

    runAssertTest(() => assert.commandFailed(sampleWriteConcernError), false);

    runAssertTest(() => assert.commandFailedWithCode(sampleWriteConcernError, ErrorCodes.DuplicateKey), true);
    runAssertTest(() => assert.commandFailedWithCode(nonTimeOutWriteConcernError, ErrorCodes.DuplicateKey), false);
    runAssertTest(() => assert.commandFailedWithCode(sampleWriteConcernError, ErrorCodes.WriteConcernTimeout), false);

    runAssertTest(() => assert.commandWorkedIgnoringWriteConcernErrors(sampleWriteConcernError), false);

    runAssertTest(() => assert.commandWorked(lockTimeoutError), true);
    runAssertTest(() => assert.commandFailed(lockTimeoutError), false);
    runAssertTest(() => assert.commandFailedWithCode(lockTimeoutError, ErrorCodes.DuplicateKey), true);
    runAssertTest(() => assert.commandFailedWithCode(lockTimeoutError, ErrorCodes.LockTimeout), false);

    runAssertTest(() => assert.commandWorked(lockTimeoutTransientTransactionError), false);
    runAssertTest(() => assert.commandFailed(lockTimeoutTransientTransactionError), false);
    runAssertTest(
        () => assert.commandFailedWithCode(lockTimeoutTransientTransactionError, ErrorCodes.DuplicateKey),
        false,
    );
    runAssertTest(
        () => assert.commandFailedWithCode(lockTimeoutTransientTransactionError, ErrorCodes.LockTimeout),
        false,
    );
});

tests.forEach((test) => {
    jsTest.log(`Starting test '${test.name}'`);
    setup();
    const oldMongoRunner = MongoRunner;
    try {
        // We shouldn't actually run the hang-analyzer for these tests.
        MongoRunner.runHangAnalyzer = Function.prototype;
        test();
    } finally {
        MongoRunner = oldMongoRunner;
    }
});

/* cleanup */
MongoRunner.stopMongod(conn);
