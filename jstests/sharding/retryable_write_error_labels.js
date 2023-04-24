/**
 * Test RetryableWriteError label in retryable writes and in transactions.
 *
 * @tags: [
 *   uses_transactions,
 * ]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");
load("jstests/libs/fail_point_util.js");

const dbName = "test";
const collName = "retryable_write_error_labels";
const ns = dbName + "." + collName;

// Use ShardingTest because we need to test both mongod and mongos behaviors
let overrideMaxAwaitTimeMS = {'mode': 'alwaysOn', 'data': {maxAwaitTimeMS: 1000}};
const st = new ShardingTest({
    config: 1,
    mongos:
        {s0: {setParameter: "failpoint.overrideMaxAwaitTimeMS=" + tojson(overrideMaxAwaitTimeMS)}},
    shards: 1
});

function checkErrorCode(res, expectedErrorCodes, isWCError) {
    // Rewrite each element of the `expectedErrorCodes` array.
    // If it's not an array, just rewrite the scalar.
    var rewrite = ec => ErrorCodes.doMongosRewrite(st.s, ec);
    if (Array.isArray(expectedErrorCodes)) {
        expectedErrorCodes = expectedErrorCodes.map(rewrite);
    } else {
        expectedErrorCodes = rewrite(expectedErrorCodes);
    }

    if (isWCError) {
        assert.neq(null, res.writeConcernError, res);
        assert(anyEq([res.writeConcernError.code], expectedErrorCodes), res);
    } else {
        assert.commandFailedWithCode(res, expectedErrorCodes);
        assert.eq(null, res.writeConcernError, res);
    }
}

function assertNotContainErrorLabels(res) {
    assert(!res.hasOwnProperty("errorLabels"), res);
}

function assertContainRetryableErrorLabel(res) {
    assert(res.hasOwnProperty("errorLabels"), res);
    assert.sameMembers(["RetryableWriteError"], res.errorLabels);
}

function enableFailCommand(node, isWCError, errorCode, commands) {
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
    return configureFailPoint(node, "failCommand", failCommandData, "alwaysOn" /*failPointMode*/);
}

function testMongodError(errorCode, isWCError) {
    const shard0Primary = st.rs0.getPrimary();
    const testDB = st.getDB(dbName);
    const session = st.s.startSession();
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);

    let insertFailPoint = enableFailCommand(shard0Primary, isWCError, errorCode, ["insert"]);

    jsTestLog(`Testing with errorCode: ${errorCode}, isWCError: ${isWCError}`);

    // Test retryable writes.
    jsTestLog("Retryable write should return error " + errorCode +
              " without RetryableWriteError label");
    let res = testDB.runCommand(
        {insert: collName, documents: [{a: errorCode, b: "retryable"}], txnNumber: NumberLong(0)});
    checkErrorCode(res, [errorCode], isWCError);
    assertNotContainErrorLabels(res);

    // Test non-retryable writes.
    jsTestLog("Non-retryable write should return error " + errorCode +
              " without RetryableWriteError label");
    res = testDB.runCommand({insert: collName, documents: [{a: errorCode, b: "non-retryable"}]});
    checkErrorCode(res, [errorCode], isWCError);
    assertNotContainErrorLabels(res);

    insertFailPoint.off();
    let commitTxnFailPoint =
        enableFailCommand(shard0Primary, isWCError, errorCode, ["commitTransaction"]);
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
    checkErrorCode(res, [errorCode], isWCError);
    assertNotContainErrorLabels(res);
    assert.commandWorkedOrFailedWithCode(
        session.abortTransaction_forTesting(),
        [ErrorCodes.TransactionCommitted, ErrorCodes.NoSuchTransaction]);

    commitTxnFailPoint.off();
    // Test abortTransaction command in a transaction.
    let abortTransactionFailPoint =
        enableFailCommand(shard0Primary, isWCError, errorCode, ["abortTransaction"]);

    jsTestLog("abortTransaction should return error " + errorCode +
              " without RetryableWriteError label");
    session.startTransaction();
    assert.commandWorked(sessionColl.update({}, {$inc: {x: 1}}));
    res = sessionDb.adminCommand({
        abortTransaction: 1,
        txnNumber: NumberLong(session.getTxnNumber_forTesting()),
        autocommit: false
    });
    checkErrorCode(res, [errorCode], isWCError);
    assertNotContainErrorLabels(res);

    abortTransactionFailPoint.off();
    assert.commandWorkedOrFailedWithCode(session.abortTransaction_forTesting(),
                                         ErrorCodes.NoSuchTransaction);
    session.endSession();
}

function testMongosError() {
    const shard0Primary = st.rs0.getPrimary();

    // Test retryable writes.
    jsTestLog("Retryable write should return mongos shutdown error with RetryableWriteError label");

    let insertFailPoint =
        configureFailPoint(shard0Primary, "hangAfterCollectionInserts", {collectionNS: ns});
    const retryableInsertThread = new Thread((mongosHost, dbName, collName) => {
        const mongos = new Mongo(mongosHost);
        const session = mongos.startSession();
        session.startTransaction();
        return session.getDatabase(dbName).runCommand({
            insert: collName,
            documents: [{a: 0, b: "retryable"}],
            txnNumber: NumberLong(session.getTxnNumber_forTesting()),
        });
    }, st.s.host, dbName, collName);
    retryableInsertThread.start();

    insertFailPoint.wait();
    MongoRunner.stopMongos(st.s);
    try {
        const retryableInsertRes = retryableInsertThread.returnData();
        checkErrorCode(retryableInsertRes,
                       [ErrorCodes.InterruptedAtShutdown, ErrorCodes.CallbackCanceled],
                       false /* isWCError */);
        assertContainRetryableErrorLabel(retryableInsertRes);
    } catch (e) {
        if (!isNetworkError(e)) {
            throw e;
        }
    }

    insertFailPoint.off();
    st.s = MongoRunner.runMongos(st.s);

    // Test non-retryable writes.
    jsTestLog(
        "Non-retryable write should return mongos shutdown error without RetryableWriteError label");
    insertFailPoint =
        configureFailPoint(shard0Primary, "hangAfterCollectionInserts", {collectionNs: ns});
    const nonRetryableInsertThread = new Thread((mongosHost, dbName, collName) => {
        const mongos = new Mongo(mongosHost);
        return mongos.getDB(dbName).runCommand({
            insert: collName,
            documents: [{a: 0, b: "non-retryable"}],
        });
    }, st.s.host, dbName, collName);
    nonRetryableInsertThread.start();
    insertFailPoint.wait();

    MongoRunner.stopMongos(st.s);
    try {
        const nonRetryableInsertRes = nonRetryableInsertThread.returnData();
        checkErrorCode(nonRetryableInsertRes,
                       [ErrorCodes.InterruptedAtShutdown, ErrorCodes.CallbackCanceled],
                       false /* isWCError */);
        assertNotContainErrorLabels(nonRetryableInsertRes);
    } catch (e) {
        if (!isNetworkError(e)) {
            throw e;
        }
    }

    insertFailPoint.off();
    st.s = MongoRunner.runMongos(st.s);

    // Test commitTransaction command.
    jsTestLog(
        "commitTransaction should return mongos shutdown error with RetryableWriteError label");
    let commitTxnFailPoint = configureFailPoint(shard0Primary, "hangBeforeCommitingTxn");
    const commitTxnThread = new Thread((mongosHost, dbName, collName) => {
        const mongos = new Mongo(mongosHost);
        const session = mongos.startSession();
        const sessionDb = session.getDatabase(dbName);
        const sessionColl = sessionDb.getCollection(collName);
        session.startTransaction();
        assert.commandWorked(sessionColl.update({}, {$inc: {x: 1}}));
        return sessionDb.adminCommand({
            commitTransaction: 1,
            txnNumber: NumberLong(session.getTxnNumber_forTesting()),
            autocommit: false
        });
    }, st.s.host, dbName, collName);
    commitTxnThread.start();

    commitTxnFailPoint.wait();
    MongoRunner.stopMongos(st.s);
    commitTxnFailPoint.off();

    try {
        const commitTxnRes = commitTxnThread.returnData();
        checkErrorCode(commitTxnRes,
                       [ErrorCodes.InterruptedAtShutdown, ErrorCodes.CallbackCanceled],
                       false /* isWCError */);
        assertContainRetryableErrorLabel(commitTxnRes);
    } catch (e) {
        if (!isNetworkError(e)) {
            throw e;
        }
    }

    st.s = MongoRunner.runMongos(st.s);

    // Test abortTransaction command.
    jsTestLog(
        "abortTransaction should return mongos shutdown error with RetryableWriteError label");
    let abortTxnFailPoint = configureFailPoint(shard0Primary, "hangBeforeAbortingTxn");
    const abortTxnThread = new Thread((mongosHost, dbName, collName) => {
        const mongos = new Mongo(mongosHost);
        const session = mongos.startSession();
        const sessionDb = session.getDatabase(dbName);
        const sessionColl = sessionDb.getCollection(collName);
        session.startTransaction();
        assert.commandWorked(sessionColl.update({}, {$inc: {x: 1}}));
        return sessionDb.adminCommand({
            abortTransaction: 1,
            txnNumber: NumberLong(session.getTxnNumber_forTesting()),
            autocommit: false
        });
    }, st.s.host, dbName, collName);
    abortTxnThread.start();

    abortTxnFailPoint.wait();
    MongoRunner.stopMongos(st.s);
    abortTxnFailPoint.off();

    try {
        const abortTxnRes = abortTxnThread.returnData();
        checkErrorCode(abortTxnRes,
                       [ErrorCodes.InterruptedAtShutdown, ErrorCodes.CallbackCanceled],
                       false /* isWCError */);
        assertContainRetryableErrorLabel(abortTxnRes);
    } catch (e) {
        if (!isNetworkError(e)) {
            throw e;
        }
    }

    st.s = MongoRunner.runMongos(st.s);
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

// mongos should never attach RetryableWriteError labels to retryable errors from shards.
retryableCodes.forEach(function(code) {
    testMongodError(code, false /* isWCError */);
});

// mongos should never attach RetryableWriteError labels to retryable writeConcern errors from
// shards.
retryableCodes.forEach(function(code) {
    testMongodError(code, true /* isWCError */);
});

// mongos should attach RetryableWriteError labels when retryable writes fail due to local
// retryable errors.
testMongosError();

st.s.adminCommand({"configureFailPoint": "overrideMaxAwaitTimeMS", "mode": "off"});

st.stop();
}());
