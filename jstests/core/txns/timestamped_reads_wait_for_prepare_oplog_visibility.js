/**
 * Tests that timestamped reads, reads with snapshot and afterClusterTime, wait for the prepare
 * transaction oplog entry to be visible before choosing a read timestamp.
 *
 * @tags: [
 *  uses_transactions,
 *  uses_prepare_transaction,
 *  uses_parallel_shell,
 *  # 'setDefaultRWConcern' is not supposed to be run on shard nodes.
 *  command_not_supported_in_serverless,
 * ]
 */
(function() {
'use strict';

load('jstests/core/txns/libs/prepare_helpers.js');
load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

TestData.dbName = 'test';
const baseCollName = 'timestamped_reads_wait_for_prepare_oplog_visibility';
const testDB = db.getSiblingDB(TestData.dbName);
TestData.failureTimeout = 1 * 1000;       // 1 second.
TestData.successTimeout = 5 * 60 * 1000;  // 5 minutes.
TestData.txnDoc = {
    _id: 1,
    x: 1
};
TestData.otherDoc = {
    _id: 2,
    y: 7
};
TestData.txnDocFilter = {
    _id: TestData.txnDoc._id
};
TestData.otherDocFilter = {
    _id: TestData.otherDoc._id
};

/**
 * A function that accepts a 'readFunc' and a collection name. 'readFunc' accepts a collection
 * name and returns an object with an 'oplogVisibility' test field and a 'prepareConflict' test
 * field. This function is run in a separate thread and tests that oplog visibility blocks
 * certain reads and that prepare conflicts block other types of reads.
 */
const readThreadFunc = function(readFunc, _collName, hangTimesEntered, logTimesEntered) {
    load("jstests/libs/fail_point_util.js");

    // Do not start reads until we are blocked in 'prepareTransaction'.
    assert.commandWorked(db.adminCommand({
        waitForFailPoint: "hangAfterReservingPrepareTimestamp",
        timesEntered: hangTimesEntered,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    // Create a 'readFuncObj' from the 'readFunc'.
    const readFuncObj = readFunc(_collName);
    readFuncObj.oplogVisibility();

    // Let the transaction finish preparing.
    assert.commandWorked(
        db.adminCommand({configureFailPoint: 'hangAfterReservingPrepareTimestamp', mode: 'off'}));

    // Wait for 'prepareTransaction' to complete and be logged.
    assert.commandWorked(db.adminCommand({
        waitForFailPoint: "waitForPrepareTransactionCommandLogged",
        timesEntered: logTimesEntered,
        maxTimeMS: kDefaultWaitForFailPointTimeout
    }));

    readFuncObj.prepareConflict();
};

function runTest(prefix, readFunc) {
    // Reset the log history between tests.
    assert.commandWorked(db.adminCommand({clearLog: 'global'}));

    try {
        // The default WC is majority and this test can't satisfy majority writes.
        assert.commandWorked(db.adminCommand(
            {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

        jsTestLog('Testing oplog visibility for ' + prefix);
        const collName = baseCollName + '_' + prefix;
        const testColl = testDB.getCollection(collName);

        testColl.drop({writeConcern: {w: "majority"}});
        assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: 'majority'}}));

        let hangFailPoint = configureFailPoint(testDB, "hangAfterReservingPrepareTimestamp");
        let logFailPoint = configureFailPoint(testDB, "waitForPrepareTransactionCommandLogged");

        // Insert a document for the transaction.
        assert.commandWorked(testColl.insert(TestData.txnDoc));
        // Insert a document untouched by the transaction.
        assert.commandWorked(testColl.insert(TestData.otherDoc, {writeConcern: {w: "majority"}}));

        // Start a transaction with a single update on the 'txnDoc'.
        const session = db.getMongo().startSession({causalConsistency: false});
        const sessionDB = session.getDatabase(TestData.dbName);
        session.startTransaction({readConcern: {level: 'snapshot'}});
        const updateResult =
            assert.commandWorked(sessionDB[collName].update(TestData.txnDoc, {$inc: {x: 1}}));
        // Make sure that txnDoc is part of both the snapshot and transaction as an update can still
        // succeed if it doesn't find any matching documents to modify.
        assert.eq(updateResult["nModified"], 1);

        // We set the log level up to know when 'prepareTransaction' completes.
        db.setLogLevel(1);

        // Clear the log history to ensure we only see the most recent 'prepareTransaction'
        // failpoint log message.
        assert.commandWorked(db.adminCommand({clearLog: 'global'}));
        const joinReadThread = startParallelShell(funWithArgs(readThreadFunc,
                                                              readFunc,
                                                              collName,
                                                              hangFailPoint.timesEntered + 1,
                                                              logFailPoint.timesEntered + 1));

        jsTestLog("Preparing the transaction for " + prefix);
        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);

        db.setLogLevel(0);
        joinReadThread({checkExitSuccess: true});

        PrepareHelpers.commitTransaction(session, prepareTimestamp);
    } finally {
        // Unsetting CWWC is not allowed, so explicitly restore the default write concern to be
        // majority by setting CWWC to {w: majority}.
        assert.commandWorked(db.adminCommand({
            setDefaultRWConcern: 1,
            defaultWriteConcern: {w: "majority"},
            writeConcern: {w: "majority"}
        }));
    }
}

const snapshotRead = function(_collName) {
    const _db = db.getSiblingDB(TestData.dbName);

    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(TestData.dbName);

    const oplogVisibility = function() {
        jsTestLog("Snapshot reads should not block on oplog visibility.");
        session.startTransaction({readConcern: {level: 'snapshot'}});
        let cursor = assert.commandWorked(sessionDB.runCommand(
            {find: _collName, filter: TestData.txnDocFilter, maxTimeMS: TestData.successTimeout}));
        assert.sameMembers(cursor.cursor.firstBatch, [TestData.txnDoc], tojson(cursor));
        assert.commandWorked(session.abortTransaction_forTesting());

        session.startTransaction({readConcern: {level: 'snapshot'}});
        cursor = assert.commandWorked(sessionDB.runCommand({
            find: _collName,
            filter: TestData.otherDocFilter,
            maxTimeMS: TestData.successTimeout
        }));
        assert.sameMembers(cursor.cursor.firstBatch, [TestData.otherDoc], tojson(cursor));
        assert.commandWorked(session.abortTransaction_forTesting());
    };

    const prepareConflict = function() {
        jsTestLog("Snapshot reads should block on prepared transactions for " +
                  "conflicting documents.");
        session.startTransaction({readConcern: {level: 'snapshot'}});
        let cursor = assert.commandFailedWithCode(sessionDB.runCommand({
            find: _collName,
            filter: TestData.txnDocFilter,
            maxTimeMS: TestData.failureTimeout
        }),
                                                  ErrorCodes.MaxTimeMSExpired);
        assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);

        jsTestLog("Snapshot reads should succeed on non-conflicting documents while a " +
                  "transaction is in prepare.");
        session.startTransaction({readConcern: {level: 'snapshot'}});
        cursor = assert.commandWorked(sessionDB.runCommand({
            find: _collName,
            filter: TestData.otherDocFilter,
            maxTimeMS: TestData.successTimeout
        }));
        assert.sameMembers(cursor.cursor.firstBatch, [TestData.otherDoc], tojson(cursor));
        assert.commandWorked(session.abortTransaction_forTesting());
    };

    return {oplogVisibility: oplogVisibility, prepareConflict: prepareConflict};
};

const afterClusterTime = function(_collName) {
    const _db = db.getSiblingDB(TestData.dbName);

    // Advance the cluster time with an arbitrary other insert.
    let res = assert.commandWorked(
        _db.runCommand({insert: _collName, documents: [{advanceClusterTime: 1}]}));
    assert(res.hasOwnProperty("$clusterTime"), tojson(res));
    assert(res.$clusterTime.hasOwnProperty("clusterTime"), tojson(res));
    const clusterTime = res.$clusterTime.clusterTime;
    jsTestLog("Using afterClusterTime: " + clusterTime);

    const oplogVisibility = function() {
        jsTestLog("afterClusterTime reads should block on oplog visibility.");
        assert.commandFailedWithCode(_db.runCommand({
            find: _collName,
            filter: TestData.txnDocFilter,
            readConcern: {afterClusterTime: clusterTime},
            maxTimeMS: TestData.failureTimeout
        }),
                                     ErrorCodes.MaxTimeMSExpired);
        assert.commandFailedWithCode(_db.runCommand({
            find: _collName,
            filter: TestData.otherDocFilter,
            readConcern: {afterClusterTime: clusterTime},
            maxTimeMS: TestData.failureTimeout
        }),
                                     ErrorCodes.MaxTimeMSExpired);
    };

    const prepareConflict = function() {
        jsTestLog("afterClusterTime reads should block on prepared transactions for " +
                  "conflicting documents.");
        assert.commandFailedWithCode(_db.runCommand({
            find: _collName,
            filter: TestData.txnDocFilter,
            readConcern: {afterClusterTime: clusterTime},
            maxTimeMS: TestData.failureTimeout
        }),
                                     ErrorCodes.MaxTimeMSExpired);

        jsTestLog("afterClusterTime reads should succeed on non-conflicting documents " +
                  "while transaction is in prepare.");
        let cursor = assert.commandWorked(_db.runCommand({
            find: _collName,
            filter: TestData.otherDocFilter,
            readConcern: {afterClusterTime: clusterTime},
            maxTimeMS: TestData.successTimeout
        }));
        assert.sameMembers(cursor.cursor.firstBatch, [TestData.otherDoc], tojson(cursor));
    };

    return {oplogVisibility: oplogVisibility, prepareConflict: prepareConflict};
};

const normalRead = function(_collName) {
    const _db = db.getSiblingDB(TestData.dbName);

    const oplogVisibility = function() {
        jsTestLog("Ordinary reads should not block on oplog visibility.");
        let cursor = assert.commandWorked(_db.runCommand(
            {find: _collName, filter: TestData.txnDocFilter, maxTimeMS: TestData.successTimeout}));
        assert.sameMembers(cursor.cursor.firstBatch, [TestData.txnDoc], tojson(cursor));
        cursor = assert.commandWorked(_db.runCommand({
            find: _collName,
            filter: TestData.otherDocFilter,
            maxTimeMS: TestData.successTimeout
        }));
        assert.sameMembers(cursor.cursor.firstBatch, [TestData.otherDoc], tojson(cursor));
    };

    const prepareConflict = function() {
        jsTestLog("Ordinary reads should not block on prepared transactions.");
        let cursor = assert.commandWorked(_db.runCommand(
            {find: _collName, filter: TestData.txnDocFilter, maxTimeMS: TestData.successTimeout}));
        assert.sameMembers(cursor.cursor.firstBatch, [TestData.txnDoc], tojson(cursor));
        cursor = assert.commandWorked(_db.runCommand({
            find: _collName,
            filter: TestData.otherDocFilter,
            maxTimeMS: TestData.successTimeout
        }));
        assert.sameMembers(cursor.cursor.firstBatch, [TestData.otherDoc], tojson(cursor));
    };

    return {oplogVisibility: oplogVisibility, prepareConflict: prepareConflict};
};

runTest('normal_reads', normalRead);
runTest('snapshot_reads', snapshotRead);
runTest('afterClusterTime', afterClusterTime);
})();
