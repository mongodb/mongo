/**
 * Tests that the transaction items in the 'twoPhaseCommitCoordinator' object in currentOp() are
 * being tracked correctly.
 * @tags: [
 *   uses_prepare_transaction,
 *   uses_transactions,
 * ]
 */

(function() {
'use strict';
load('jstests/libs/fail_point_util.js');
load('jstests/sharding/libs/sharded_transactions_helpers.js');  // for waitForFailpoint

function commitTxn(st, lsid, txnNumber, expectedError = null) {
    let cmd = "db.adminCommand({" +
        "commitTransaction: 1," +
        "lsid: " + tojson(lsid) + "," +
        "txnNumber: NumberLong(" + txnNumber + ")," +
        "stmtId: NumberInt(0)," +
        "autocommit: false})";

    if (expectedError) {
        cmd = "assert.commandFailedWithCode(" + cmd + "," + String(expectedError) + ");";
    } else {
        cmd = "assert.commandWorked(" + cmd + ");";
    }

    return startParallelShell(cmd, st.s.port);
}

function curOpAfterFailpoint(failPoint, filter, timesEntered = 1) {
    jsTest.log(`waiting for failpoint '${failPoint.failPointName}' to be entered ${
        timesEntered} time(s).`);
    if (timesEntered > 1) {
        const expectedLog = "Hit " + failPoint.failPointName + " failpoint";
        waitForFailpoint(expectedLog, timesEntered);
    } else {
        failPoint.wait();
    }

    jsTest.log(`Running curOp operation after '${failPoint.failPointName}' failpoint.`);
    let result =
        adminDB.aggregate([{$currentOp: {'idleConnections': true}}, {$match: filter}]).toArray();

    jsTest.log(`${result.length} matching curOp entries after '${failPoint.failPointName}':\n${
        tojson(result)}`);

    jsTest.log(`disable '${failPoint.failPointName}' failpoint.`);
    failPoint.off();

    return result;
}

function makeWorkerFilterWithAction(session, action, txnNumber, txnRetryCounter) {
    var filter = {
        'twoPhaseCommitCoordinator.lsid.id': session.getSessionId().id,
        'twoPhaseCommitCoordinator.txnNumber': NumberLong(txnNumber),
        'twoPhaseCommitCoordinator.action': action,
        'twoPhaseCommitCoordinator.startTime': {$exists: true}
    };

    Object.assign(filter,
                  {'twoPhaseCommitCoordinator.txnRetryCounter': NumberInt(txnRetryCounter)});

    return filter;
}

function enableFailPoints(shard, failPointNames) {
    let failPoints = {};

    jsTest.log(`enabling the following failpoints: ${tojson(failPointNames)}`);
    failPointNames.forEach(function(failPointName) {
        failPoints[failPointName] = configureFailPoint(shard, failPointName);
    });

    return failPoints;
}

function startTransaction(session, collectionName, insertValue) {
    const dbName = session.getDatabase('test');
    jsTest.log(`Starting a new transaction on ${dbName}.${collectionName}`);
    var insertCmdObj = {
        insert: collectionName,
        documents: [{_id: -1 * insertValue}, {_id: insertValue}],
        lsid: session.getSessionId(),
        txnNumber: NumberLong(1),
        startTransaction: true,
        autocommit: false,
    };

    assert.commandWorked(dbName.runCommand(insertCmdObj));
}

// Setup test
const numShards = 2;
const st = new ShardingTest({shards: numShards, config: 1});
const dbName = "test";
const collectionName = 'currentop_two_phase';
const ns = dbName + "." + collectionName;
const adminDB = st.s.getDB('admin');
const coordinator = st.shard0;
const participant = st.shard1;
const failPointNames = [
    'hangAfterStartingCoordinateCommit',
    'hangBeforeWritingParticipantList',
    'hangBeforeSendingPrepare',
    'hangBeforeWritingDecision',
    'hangBeforeSendingCommit',
    'hangBeforeDeletingCoordinatorDoc',
    'hangBeforeSendingAbort'
];

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: coordinator.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant.shardName}));
assert.commandWorked(coordinator.adminCommand({_flushRoutingTableCacheUpdates: ns}));
assert.commandWorked(participant.adminCommand({_flushRoutingTableCacheUpdates: ns}));
st.refreshCatalogCacheForNs(st.s, ns);

let failPoints = enableFailPoints(coordinator, failPointNames);

jsTest.log("Testing that coordinator threads show up in currentOp for a commit decision");
{
    let session = adminDB.getMongo().startSession();
    startTransaction(session, collectionName, 1);
    let txnNumber = NumberLong(1);
    let txnRetryCounter = NumberInt(0);
    let lsid = session.getSessionId();
    let commitJoin = commitTxn(st, lsid, txnNumber);

    var coordinateCommitFilter = {
        active: true,
        'command.coordinateCommitTransaction': 1,
        'command.lsid.id': session.getSessionId().id,
        'command.txnNumber': txnNumber,
        'command.coordinator': true,
        'command.autocommit': false
    };

    let createCoordinateCommitTxnOp = curOpAfterFailpoint(
        failPoints["hangAfterStartingCoordinateCommit"], coordinateCommitFilter);
    assert.eq(1, createCoordinateCommitTxnOp.length);

    const writeParticipantFilter =
        makeWorkerFilterWithAction(session, "writingParticipantList", txnNumber, txnRetryCounter);
    let writeParticipantOp =
        curOpAfterFailpoint(failPoints['hangBeforeWritingParticipantList'], writeParticipantFilter);
    assert.eq(1, writeParticipantOp.length);

    const sendPrepareFilter =
        makeWorkerFilterWithAction(session, "sendingPrepare", txnNumber, txnRetryCounter);
    let sendPrepareOp =
        curOpAfterFailpoint(failPoints['hangBeforeSendingPrepare'], sendPrepareFilter, numShards);
    assert.eq(numShards, sendPrepareOp.length);

    const writingDecisionFilter =
        makeWorkerFilterWithAction(session, "writingDecision", txnNumber, txnRetryCounter);
    let writeDecisionOp =
        curOpAfterFailpoint(failPoints['hangBeforeWritingDecision'], writingDecisionFilter);
    assert.eq(1, writeDecisionOp.length);

    const sendCommitFilter =
        makeWorkerFilterWithAction(session, "sendingCommit", txnNumber, txnRetryCounter);
    let sendCommitOp =
        curOpAfterFailpoint(failPoints['hangBeforeSendingCommit'], sendCommitFilter, numShards);
    assert.eq(numShards, sendCommitOp.length);

    const deletingCoordinatorFilter =
        makeWorkerFilterWithAction(session, "deletingCoordinatorDoc", txnNumber, txnRetryCounter);
    let deletingCoordinatorDocOp = curOpAfterFailpoint(
        failPoints['hangBeforeDeletingCoordinatorDoc'], deletingCoordinatorFilter);
    assert.eq(1, deletingCoordinatorDocOp.length);

    commitJoin();
}

jsTest.log("Testing that coordinator threads show up in currentOp for an abort decision.");
{
    let session = adminDB.getMongo().startSession();
    startTransaction(session, collectionName, 2);
    let txnNumber = NumberLong(1);
    let lsid = session.getSessionId();
    let txnRetryCounter = NumberInt(0);
    // Manually abort the transaction on one of the participants, so that the participant fails to
    // prepare and failpoint is triggered on the coordinator.
    var abortTransactionCmd = {
        abortTransaction: 1,
        lsid: lsid,
        txnNumber: txnNumber,
        stmtId: NumberInt(0),
        autocommit: false,
    };

    assert.commandWorked(participant.adminCommand(abortTransactionCmd));

    let commitJoin = commitTxn(st, lsid, txnNumber, ErrorCodes.NoSuchTransaction);

    const sendAbortFilter =
        makeWorkerFilterWithAction(session, "sendingAbort", txnNumber, txnRetryCounter);
    let sendingAbortOp =
        curOpAfterFailpoint(failPoints['hangBeforeSendingAbort'], sendAbortFilter, numShards);
    assert.eq(numShards, sendingAbortOp.length);

    commitJoin();
}

st.stop();
})();
