/**
 * Test RetryableWriteError label in retryable writes and in transactions.
 *
 * The "requires_find_command" tag excludes this test from the op_query suites, which are
 * incompatible with implicit sessions.
 *
 * @tags: [
 *   requires_find_command,
 *   uses_transactions,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

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

function checkErrorCode(res, errorCode, isWCError) {
    if (isWCError) {
        assert.neq(null, res.writeConcernError, res);
        assert.eq(res.writeConcernError.code, errorCode, res);
    } else {
        assert.commandFailedWithCode(res, errorCode);
        assert.eq(null, res.writeConcernError, res);
    }
}

function checkErrorLabels(res) {
    assert(!res.hasOwnProperty("errorLabels"), res);
}

function enableFailCommand(isWCError, errorCode, commands) {
    jsTestLog("Enabling failCommand fail point for " + commands + " with writeConcern error " +
              isWCError);
    // Sharding tests require {failInternalCommands: true},
    // s appears to mongod to be an internal client.
    let failCommandData = {failInternalCommands: true, failCommands: commands};
    if (isWCError) {
        failCommandData['writeConcernError'] = {code: NumberInt(errorCode), errmsg: "dummy"};
    } else {
        failCommandData['errorCode'] = NumberInt(errorCode);
    }
    return configureFailPoint(
        st.rs0.getPrimary(), "failCommand", failCommandData, "alwaysOn" /*failPointMode*/);
}

function runTest(errorCode, isWCError) {
    const testDB = st.getDB(dbName);
    const session = st.s.startSession();
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);

    let insertFailPoint = enableFailCommand(isWCError, errorCode, ["insert"]);

    jsTestLog(`Testing with errorCode: ${errorCode}, isWCError: ${isWCError}`);

    // Test retryable writes.
    jsTestLog("Retryable write should return error " + errorCode +
              " without RetryableWriteError label");
    let res = testDB.runCommand(
        {insert: collName, documents: [{a: errorCode, b: "retryable"}], txnNumber: NumberLong(0)});
    checkErrorCode(res, errorCode, isWCError);
    checkErrorLabels(res);

    // Test non-retryable writes.
    jsTestLog("Non-retryable write should return error " + errorCode +
              " without RetryableWriteError label");
    res = testDB.runCommand({insert: collName, documents: [{a: errorCode, b: "non-retryable"}]});
    checkErrorCode(res, errorCode, isWCError);
    checkErrorLabels(res);

    insertFailPoint.off();
    let commitTxnFailPoint = enableFailCommand(isWCError, errorCode, ["commitTransaction"]);
    // Test commitTransaction command in a transaction.
    jsTestLog("commitTransaction should return error " + errorCode +
              " without RetryableWriteError label");
    session.startTransaction();
    assert.commandWorked(sessionColl.update({}, {$inc: {x: 1}}));
    res = sessionDb.adminCommand({
        commitTransaction: 1,
        txnNumber: NumberLong(session.getTxnNumber_forTesting()),
        autocommit: false
    });
    checkErrorCode(res, errorCode, isWCError);
    checkErrorLabels(res);
    assert.commandWorkedOrFailedWithCode(
        session.abortTransaction_forTesting(),
        [ErrorCodes.TransactionCommitted, ErrorCodes.NoSuchTransaction]);

    commitTxnFailPoint.off();
    // Test abortTransaction command in a transaction.
    let abortTransactionFailPoint = enableFailCommand(isWCError, errorCode, ["abortTransaction"]);

    jsTestLog("abortTransaction should return error " + errorCode +
              " without RetryableWriteError label");
    session.startTransaction();
    assert.commandWorked(sessionColl.update({}, {$inc: {x: 1}}));
    res = sessionDb.adminCommand({
        abortTransaction: 1,
        txnNumber: NumberLong(session.getTxnNumber_forTesting()),
        autocommit: false
    });
    checkErrorCode(res, errorCode, isWCError);
    checkErrorLabels(res);

    abortTransactionFailPoint.off();
    assert.commandWorkedOrFailedWithCode(session.abortTransaction_forTesting(),
                                         ErrorCodes.NoSuchTransaction);
    session.endSession();
}

const retryableCodes = [
    ErrorCodes.InterruptedAtShutdown,
    ErrorCodes.InterruptedDueToReplStateChange,
    ErrorCodes.NotWritablePrimary,
    ErrorCodes.NotPrimaryNoSecondaryOk,
    ErrorCodes.NotPrimaryOrSecondary,
    ErrorCodes.PrimarySteppedDown,
    ErrorCodes.ShutdownInProgress,
    ErrorCodes.HostNotFound,
    ErrorCodes.HostUnreachable,
    ErrorCodes.NetworkTimeout,
    ErrorCodes.SocketException,
    ErrorCodes.ExceededTimeLimit,
    ErrorCodes.WriteConcernFailed
];

// Test retryable error codes.
retryableCodes.forEach(function(code) {
    // Mongos should never return RetryableWriteError labels.
    runTest(code, false /* isWCError */);
});

// Test retryable error codes in writeConcern error.
retryableCodes.forEach(function(code) {
    // Mongos should never return RetryableWriteError labels.
    runTest(code, true /* isWCError */);
});

st.s.adminCommand({"configureFailPoint": "overrideMaxAwaitTimeMS", "mode": "off"});

st.stop();
}());
