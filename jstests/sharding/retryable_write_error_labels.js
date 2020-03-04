/**
 * Test RetryableWriteError label in retryable writes and in transactions.
 *
 * The "requires_find_command" tag excludes this test from the op_query suites, which are
 * incompatible with implicit sessions.
 *
 * @tags: [uses_transactions, requires_fcv_44, requires_find_command]
 */

(function() {
"use strict";

const dbName = "test";
const collName = "retryable_write_error_labels";

// Use ShardingTest because we need to test both mongod and mongos behaviors
let overrideMaxAwaitTimeMS = {'mode': 'alwaysOn', 'data': {maxAwaitTimeMS: 1000}};
const st = new ShardingTest({
    config: 1,
    mongos:
        {s0: {setParameter: "failpoint.overrideMaxAwaitTimeMS=" + tojson(overrideMaxAwaitTimeMS)}},

    shards: 1
});
const primary = st.rs0.getPrimary();

assert.commandWorked(primary.getDB(dbName).runCommand(
    {insert: collName, documents: [{_id: 0}], writeConcern: {w: "majority"}}));

function checkErrorCode(res, errorCode, isWCError) {
    if (isWCError) {
        assert.neq(null, res.writeConcernError, res);
        assert.eq(res.writeConcernError.code, errorCode, res);
    } else {
        assert.commandFailedWithCode(res, errorCode);
        assert.eq(null, res.writeConcernError, res);
    }
}

function checkErrorLabels(res, expectLabel) {
    if (expectLabel) {
        assert.eq(res.errorLabels, ["RetryableWriteError"], res);
    } else {
        assert(!res.hasOwnProperty("errorLabels"), res);
    }
}

function runTest(errorCode, expectLabel, isWCError, isMongos) {
    const testDB = isMongos ? st.s.getDB(dbName) : primary.getDB(dbName);
    const session = isMongos ? st.s.startSession() : primary.startSession();
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);

    jsTestLog(`Testing with errorCode: ${errorCode}, expectLabel: ${expectLabel}, isWCError: ${
        isWCError}, isMongos: ${isMongos}`);

    // Sharding tests (i.e. isMongos is true) require {failInternalCommands: true}, since the mongos
    // appears to mongod to be an internal client.
    if (isWCError) {
        assert.commandWorked(primary.adminCommand({
            configureFailPoint: "failCommand",
            mode: "alwaysOn",
            data: {
                writeConcernError: {code: NumberInt(errorCode), errmsg: "dummy"},
                failCommands: ["insert", "commitTransaction"],
                failInternalCommands: isMongos
            }
        }));
    } else {
        assert.commandWorked(primary.adminCommand({
            configureFailPoint: "failCommand",
            mode: "alwaysOn",
            data: {
                errorCode: NumberInt(errorCode),
                failCommands: ["insert", "commitTransaction"],
                failInternalCommands: isMongos
            }
        }));
    }

    const withOrWithout = expectLabel ? " with" : " without";

    // Test retryable writes.
    jsTestLog("Retryable write should return error " + errorCode + withOrWithout +
              " RetryableWriteError label");
    let res = testDB.runCommand(
        {insert: collName, documents: [{a: errorCode, b: "retryable"}], txnNumber: NumberLong(0)});
    checkErrorCode(res, errorCode, isWCError);
    checkErrorLabels(res, expectLabel);

    // Test non-retryable writes.
    jsTestLog("Non-retryable write should return error " + errorCode +
              " without RetryableWriteError label");
    res = testDB.runCommand({insert: collName, documents: [{a: errorCode, b: "non-retryable"}]});
    checkErrorCode(res, errorCode, isWCError);
    checkErrorLabels(res, false /* expectLabel */);

    // Test commitTransaction command in a transaction.
    jsTestLog("commitTransaction should return error " + errorCode + withOrWithout +
              " RetryableWriteError label");
    session.startTransaction();
    assert.commandWorked(sessionColl.update({}, {$inc: {x: 1}}));
    res = sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(session.getTxnNumber_forTesting()),
        autocommit: false
    });
    checkErrorCode(res, errorCode, isWCError);
    checkErrorLabels(res, expectLabel);
    assert.commandWorkedOrFailedWithCode(
        session.abortTransaction_forTesting(),
        [ErrorCodes.TransactionCommitted, ErrorCodes.NoSuchTransaction]);

    // Test abortTransaction command in a transaction.
    if (isWCError) {
        assert.commandWorked(primary.adminCommand({
            configureFailPoint: "failCommand",
            mode: "alwaysOn",
            data: {
                writeConcernError: {code: NumberInt(errorCode), errmsg: "dummy"},
                failCommands: ["abortTransaction"],
                failInternalCommands: isMongos
            }
        }));
    } else {
        assert.commandWorked(primary.adminCommand({
            configureFailPoint: "failCommand",
            mode: "alwaysOn",
            data: {
                errorCode: NumberInt(errorCode),
                failCommands: ["abortTransaction"],
                failInternalCommands: isMongos
            }
        }));
    }
    jsTestLog("abortTransaction should return error " + errorCode + withOrWithout +
              " RetryableWriteError label");
    session.startTransaction();
    assert.commandWorked(sessionColl.update({}, {$inc: {x: 1}}));
    res = sessionDb.adminCommand({
        abortTransaction: 1,
        txnNumber: NumberLong(session.getTxnNumber_forTesting()),
        autocommit: false
    });
    checkErrorCode(res, errorCode, isWCError);
    checkErrorLabels(res, expectLabel);

    assert.commandWorked(primary.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    assert.commandWorkedOrFailedWithCode(session.abortTransaction_forTesting(),
                                         ErrorCodes.NoSuchTransaction);
    session.endSession();
}

const retryableCodes = [
    ErrorCodes.InterruptedAtShutdown,
    ErrorCodes.InterruptedDueToReplStateChange,
    ErrorCodes.NotMaster,
    ErrorCodes.NotMasterNoSlaveOk,
    ErrorCodes.NotMasterOrSecondary,
    ErrorCodes.PrimarySteppedDown,
    ErrorCodes.ShutdownInProgress,
    ErrorCodes.HostNotFound,
    ErrorCodes.HostUnreachable,
    ErrorCodes.NetworkTimeout,
    ErrorCodes.SocketException,
    ErrorCodes.ExceededTimeLimit
];

// Test retryable error codes.
retryableCodes.forEach(function(code) {
    // Mongod should return RetryableWriteError labels on retryable error codes.
    runTest(code, true /* expectLabel */, false /* isWCError */, false /* isMongos */);

    // Mongos should never return RetryableWriteError labels.
    runTest(code, false /* expectLabel */, false /* isWCError */, true /* isMongos */);
});

// Test retryable error codes in writeConcern error.
retryableCodes.forEach(function(code) {
    // Mongod should return RetryableWriteError labels on retryable error codes.
    runTest(code, true /* expectLabel */, true /* isWCError */, false /* isMongos */);

    // Mongos should never return RetryableWriteError labels.
    runTest(code, false /* expectLabel */, true /* isWCError */, true /* isMongos */);
});

// Test non-retryable error code.
// Test against mongod.
runTest(ErrorCodes.WriteConcernFailed,
        false /* expectLabel */,
        false /* isWCError */,
        false /* isMongos */);
// Test against mongos.
runTest(ErrorCodes.WriteConcernFailed,
        false /* expectLabel */,
        false /* isWCError */,
        true /* isMongos */);

// Test non-retryable error code in writeConcern error.
// Test against mongod.
runTest(ErrorCodes.WriteConcernFailed,
        false /* expectLabel */,
        true /* isWCError */,
        false /* isMongos */);
// Test against mongos.
runTest(ErrorCodes.WriteConcernFailed,
        false /* expectLabel */,
        true /* isWCError */,
        true /* isMongos */);

st.s.adminCommand({"configureFailPoint": "overrideMaxAwaitTimeMS", "mode": "off"});

st.stop();
}());
