/**
 * Test RetryableWriteError label in retryable writes and in transactions.
 *
 * @tags: [
 *   uses_transactions,
 * ]
 */
import {anyEq} from "jstests/aggregation/extras/utils.js";
import {
    withAbortAndRetryOnTransientTxnError,
    withRetryOnTransientTxnError,
} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const collName = "retryable_write_error_labels";
const ns = dbName + "." + collName;
const acceptableErrorsDuringShutdown = [
    ErrorCodes.InterruptedAtShutdown,
    ErrorCodes.CallbackCanceled,
    ErrorCodes.ShutdownInProgress,
];

// Use ShardingTest because we need to test both mongod and mongos behaviors
let overrideMaxAwaitTimeMS = {"mode": "alwaysOn", "data": {maxAwaitTimeMS: 1000}};
const st = new ShardingTest({
    config: 1,
    mongos: {s0: {setParameter: {"failpoint.overrideMaxAwaitTimeMS": tojson(overrideMaxAwaitTimeMS)}}},
    shards: 1,
});

const isUnifiedWriteExecutor = st.s.adminCommand({
    getParameter: 1,
    internalQueryUnifiedWriteExecutor: 1,
}).internalQueryUnifiedWriteExecutor;

function checkErrorCode(res, expectedErrorCodes, isWCError) {
    // Rewrite each element of the `expectedErrorCodes` array.
    // If it's not an array, just rewrite the scalar.
    let rewrite = (ec) => ErrorCodes.doMongosRewrite(st.s, ec);
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
    jsTestLog("Enabling failCommand fail point for " + commands + " with writeConcern error " + isWCError);
    // Sharding tests require {failInternalCommands: true},
    // s appears to mongod to be an internal client.
    let failCommandData = {failInternalCommands: true, failCommands: commands};
    if (isWCError) {
        failCommandData["writeConcernError"] = {code: NumberInt(errorCode), errmsg: "dummy"};
    } else {
        failCommandData["errorCode"] = NumberInt(errorCode);
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
    if (isUnifiedWriteExecutor) {
        // In the unified write executor batched writes are converted to bulkWrites.
        insertFailPoint = enableFailCommand(shard0Primary, isWCError, errorCode, ["bulkWrite"]);
    }

    jsTestLog(`Testing with errorCode: ${errorCode}, isWCError: ${isWCError}`);

    // Test retryable writes.
    jsTestLog("Retryable write should return error " + errorCode + " without RetryableWriteError label");
    let res = testDB.runCommand({
        insert: collName,
        documents: [{a: errorCode, b: "retryable"}],
        txnNumber: NumberLong(0),
    });
    checkErrorCode(res, [errorCode], isWCError);
    assertNotContainErrorLabels(res);

    // Test non-retryable writes.
    jsTestLog("Non-retryable write should return error " + errorCode + " without RetryableWriteError label");
    res = testDB.runCommand({insert: collName, documents: [{a: errorCode, b: "non-retryable"}]});
    checkErrorCode(res, [errorCode], isWCError);
    assertNotContainErrorLabels(res);

    insertFailPoint.off();
    let commitTxnFailPoint = enableFailCommand(shard0Primary, isWCError, errorCode, ["commitTransaction"]);
    // Test commitTransaction command in a transaction.
    jsTestLog("commitTransaction should return error " + errorCode + " without RetryableWriteError label");
    withAbortAndRetryOnTransientTxnError(session, () => {
        session.startTransaction();
        assert.commandWorked(sessionColl.update({}, {$inc: {x: 1}}));
        res = sessionDb.adminCommand({
            commitTransaction: 1,
            txnNumber: NumberLong(session.getTxnNumber_forTesting()),
            autocommit: false,
        });
    });
    checkErrorCode(res, [errorCode], isWCError);
    assertNotContainErrorLabels(res);
    assert.commandWorkedOrFailedWithCode(session.abortTransaction_forTesting(), [
        ErrorCodes.TransactionCommitted,
        ErrorCodes.NoSuchTransaction,
    ]);

    commitTxnFailPoint.off();
    // Test abortTransaction command in a transaction.
    let abortTransactionFailPoint = enableFailCommand(shard0Primary, isWCError, errorCode, ["abortTransaction"]);

    jsTestLog("abortTransaction should return error " + errorCode + " without RetryableWriteError label");
    withAbortAndRetryOnTransientTxnError(session, () => {
        session.startTransaction();
        assert.commandWorked(sessionColl.update({}, {$inc: {x: 1}}));
        res = sessionDb.adminCommand({
            abortTransaction: 1,
            txnNumber: NumberLong(session.getTxnNumber_forTesting()),
            autocommit: false,
        });
    });
    checkErrorCode(res, [errorCode], isWCError);
    assertNotContainErrorLabels(res);

    abortTransactionFailPoint.off();
    assert.commandWorkedOrFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
    session.endSession();
}

function testMongosError() {
    const shard0Primary = st.rs0.getPrimary();

    // Insert initial documents used by the test.
    const docs = [
        {k: 0, x: 0},
        {k: 1, x: 1},
    ];
    assert.commandWorked(shard0Primary.getDB(dbName)[collName].insert(docs));

    // Test retryable writes.
    jsTestLog("Retryable write should return mongos shutdown error with RetryableWriteError label");

    let insertFailPoint;
    withRetryOnTransientTxnError(
        () => {
            insertFailPoint = configureFailPoint(shard0Primary, "hangAfterCollectionInserts", {collectionNS: ns});
            const retryableInsertThread = new Thread(
                (mongosHost, dbName, collName) => {
                    const mongos = new Mongo(mongosHost);
                    const session = mongos.startSession();
                    session.startTransaction();
                    return session.getDatabase(dbName).runCommand({
                        insert: collName,
                        documents: [{a: 0, b: "retryable"}],
                        txnNumber: NumberLong(session.getTxnNumber_forTesting()),
                    });
                },
                st.s.host,
                dbName,
                collName,
            );
            retryableInsertThread.start();

            insertFailPoint.wait();
            MongoRunner.stopMongos(st.s);
            try {
                const retryableInsertRes = retryableInsertThread.returnData();
                checkErrorCode(retryableInsertRes, acceptableErrorsDuringShutdown, false /* isWCError */);
                assertContainRetryableErrorLabel(retryableInsertRes);
            } catch (e) {
                if (!isNetworkError(e)) {
                    throw e;
                }
            }

            insertFailPoint.off();
            st.s = MongoRunner.runMongos(st.s);
        },
        () => {
            insertFailPoint.off();
            st.s = MongoRunner.runMongos(st.s);
        },
    );

    // Test non-retryable writes.
    jsTestLog("Non-retryable write should return mongos shutdown error without RetryableWriteError label");
    insertFailPoint = configureFailPoint(shard0Primary, "hangAfterCollectionInserts", {collectionNs: ns});
    const nonRetryableInsertThread = new Thread(
        (mongosHost, dbName, collName) => {
            const mongos = new Mongo(mongosHost);
            return mongos.getDB(dbName).runCommand({
                insert: collName,
                documents: [{a: 0, b: "non-retryable"}],
            });
        },
        st.s.host,
        dbName,
        collName,
    );
    nonRetryableInsertThread.start();
    insertFailPoint.wait();

    MongoRunner.stopMongos(st.s);
    try {
        const nonRetryableInsertRes = nonRetryableInsertThread.returnData();
        checkErrorCode(nonRetryableInsertRes, acceptableErrorsDuringShutdown, false /* isWCError */);
        assertNotContainErrorLabels(nonRetryableInsertRes);
    } catch (e) {
        if (!isNetworkError(e)) {
            throw e;
        }
    }

    insertFailPoint.off();
    st.s = MongoRunner.runMongos(st.s);

    // Test commitTransaction command.
    jsTestLog("commitTransaction should return mongos shutdown error with RetryableWriteError label");
    const mongosConn = new Mongo(st.s.host);
    const session = mongosConn.startSession();
    let commitTxnFailPoint = assert.commandWorked(
        shard0Primary.getDB("admin").runCommand({
            configureFailPoint: "hangBeforeCommitingTxn",
            mode: "alwaysOn",
            data: {uuid: session.getSessionId().id},
        }),
    );
    let timesEntered = commitTxnFailPoint.count;
    const shutdownThread = new Thread(
        (mongos, shard0PrimaryHost, timesEntered) => {
            let primary = new Mongo(shard0PrimaryHost);
            const kDefaultWaitForFailPointTimeout = 10 * 60 * 1000;
            assert.commandWorked(
                primary.getDB("admin").runCommand({
                    waitForFailPoint: "hangBeforeCommitingTxn",
                    timesEntered: timesEntered + 1,
                    maxTimeMS: kDefaultWaitForFailPointTimeout,
                }),
            );
            MongoRunner.stopMongos(mongos);
            assert.commandWorked(
                primary.getDB("admin").runCommand({configureFailPoint: "hangBeforeCommitingTxn", mode: "off"}),
            );
        },
        st.s,
        st.rs0.getPrimary().host,
        timesEntered,
    );
    shutdownThread.start();

    withRetryOnTransientTxnError(
        () => {
            session.startTransaction();
            const sessionDb = session.getDatabase(dbName);
            const sessionColl = sessionDb.getCollection(collName);
            assert.commandWorked(sessionColl.update({k: 0}, {$inc: {x: 1}}));
            const commitTxnRes = sessionDb.adminCommand({
                commitTransaction: 1,
                txnNumber: session.getTxnNumber_forTesting(),
                autocommit: false,
            });

            try {
                checkErrorCode(commitTxnRes, acceptableErrorsDuringShutdown, false /* isWCError */);
                assertContainRetryableErrorLabel(commitTxnRes);
            } catch (e) {
                if (!isNetworkError(e)) {
                    throw e;
                }
            }
        },
        () => {
            session.abortTransaction();
        },
    );

    shutdownThread.join();
    mongosConn.close();

    st.s = MongoRunner.runMongos(st.s);

    // Test abortTransaction command.
    jsTestLog("abortTransaction should return mongos shutdown error with RetryableWriteError label");
    let abortTxnFailPoint;
    withRetryOnTransientTxnError(
        () => {
            abortTxnFailPoint = configureFailPoint(shard0Primary, "hangBeforeAbortingTxn");
            const abortTxnThread = new Thread(
                (mongosHost, dbName, collName) => {
                    const mongos = new Mongo(mongosHost);
                    const session = mongos.startSession();
                    const sessionDb = session.getDatabase(dbName);
                    const sessionColl = sessionDb.getCollection(collName);
                    session.startTransaction();
                    assert.commandWorked(sessionColl.update({k: 1}, {$inc: {x: 1}}));
                    return sessionDb.adminCommand({
                        abortTransaction: 1,
                        txnNumber: NumberLong(session.getTxnNumber_forTesting()),
                        autocommit: false,
                    });
                },
                st.s.host,
                dbName,
                collName,
            );
            abortTxnThread.start();

            abortTxnFailPoint.wait();
            MongoRunner.stopMongos(st.s);
            abortTxnFailPoint.off();

            try {
                const abortTxnRes = abortTxnThread.returnData();
                checkErrorCode(abortTxnRes, acceptableErrorsDuringShutdown, false /* isWCError */);
                assertContainRetryableErrorLabel(abortTxnRes);
            } catch (e) {
                if (!isNetworkError(e)) {
                    throw e;
                }
            }

            st.s = MongoRunner.runMongos(st.s);
        },
        () => {
            abortTxnFailPoint.off();
            st.s = MongoRunner.runMongos(st.s);
        },
    );
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
    ErrorCodes.WriteConcernTimeout,
];

// mongos should never attach RetryableWriteError labels to retryable errors from shards.
retryableCodes.forEach(function (code) {
    testMongodError(code, false /* isWCError */);
});

// mongos should never attach RetryableWriteError labels to retryable writeConcern errors from
// shards.
retryableCodes.forEach(function (code) {
    testMongodError(code, true /* isWCError */);
});

// mongos should attach RetryableWriteError labels when retryable writes fail due to local
// retryable errors.
testMongosError();

st.s.adminCommand({"configureFailPoint": "overrideMaxAwaitTimeMS", "mode": "off"});

st.stop();
