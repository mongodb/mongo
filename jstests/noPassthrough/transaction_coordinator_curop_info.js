/**
 * Tests that the transaction items in the 'twoPhaseCommitCoordinator' object in currentOp() are
 * being tracked correctly.
 * @tags: [uses_transactions, uses_prepare_transaction]
 */

(function() {
'use strict';
load('jstests/sharding/libs/sharded_transactions_helpers.js');

function commitTxn(st, lsid, txnNumber, expectedError = null) {
    let cmd = "db.adminCommand({" +
        "commitTransaction: 1," +
        "lsid: " + tojson(lsid) + "," +
        "txnNumber: NumberLong(" + txnNumber + ")," +
        "stmtId: NumberInt(0)," +
        "autocommit: false," +
        "})";
    if (expectedError) {
        cmd = "assert.commandFailedWithCode(" + cmd + "," + String(expectedError) + ");";
    } else {
        cmd = "assert.commandWorked(" + cmd + ");";
    }
    return startParallelShell(cmd, st.s.port);
}

function curOpAfterFailpoint(fpName, filter, fpCount = 1) {
    const expectedLog = "Hit " + fpName + " failpoint";
    jsTest.log(`waiting for failpoint '${fpName}' to appear in the log ${fpCount} time(s).`);
    waitForFailpoint(expectedLog, fpCount);

    jsTest.log(`Running curOp operation after '${fpName}' failpoint.`);
    let result = adminDB.aggregate([{$currentOp: {}}, {$match: filter}]).toArray();

    jsTest.log(`${result.length} matching curOp entries after '${fpName}':\n${tojson(result)}`);

    jsTest.log(`disable '${fpName}' failpoint.`);
    assert.commandWorked(coordinator.adminCommand({
        configureFailPoint: fpName,
        mode: "off",
    }));

    return result;
}

function makeWorkerFilterWithAction(session, action, txnNumber) {
    return {
        active: true,
        'twoPhaseCommitCoordinator.lsid.id': session.getSessionId().id,
        'twoPhaseCommitCoordinator.txnNumber': NumberLong(txnNumber),
        'twoPhaseCommitCoordinator.action': action,
        'twoPhaseCommitCoordinator.startTime': {$exists: true}
    };
}

function enableFailPoints(shard, failPoints) {
    jsTest.log(`enabling the following failpoints: ${tojson(failPoints)}`);
    failPoints.forEach(function(fpName) {
        assert.commandWorked(shard.adminCommand({
            configureFailPoint: fpName,
            mode: "alwaysOn",
        }));
    });
}

function startTransaction(session, collectionName, insertValue) {
    const dbName = session.getDatabase('test');
    jsTest.log(`Starting a new transaction on ${dbName}.${collectionName}`);
    session.startTransaction();
    // insert into both shards
    assert.commandWorked(dbName[collectionName].insert({_id: -1 * insertValue}));
    assert.commandWorked(dbName[collectionName].insert({_id: insertValue}));
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
const failPoints = [
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

enableFailPoints(coordinator, failPoints);

jsTest.log("Testing that coordinator threads show up in currentOp for a commit decision");
{
    let session = adminDB.getMongo().startSession();
    startTransaction(session, collectionName, 1);
    let txnNumber = session.getTxnNumber_forTesting();
    let lsid = session.getSessionId();
    let commitJoin = commitTxn(st, lsid, txnNumber);

    const coordinateCommitFilter = {
        active: true,
        'command.coordinateCommitTransaction': 1,
        'command.lsid.id': session.getSessionId().id,
        'command.txnNumber': NumberLong(txnNumber),
        'command.coordinator': true,
        'command.autocommit': false
    };
    let createCoordinateCommitTxnOp =
        curOpAfterFailpoint("hangAfterStartingCoordinateCommit", coordinateCommitFilter);
    assert.eq(1, createCoordinateCommitTxnOp.length);

    const writeParticipantFilter =
        makeWorkerFilterWithAction(session, "writingParticipantList", txnNumber);
    let writeParticipantOp =
        curOpAfterFailpoint('hangBeforeWritingParticipantList', writeParticipantFilter);
    assert.eq(1, writeParticipantOp.length);

    const sendPrepareFilter = makeWorkerFilterWithAction(session, "sendingPrepare", txnNumber);
    let sendPrepareOp =
        curOpAfterFailpoint('hangBeforeSendingPrepare', sendPrepareFilter, numShards);
    assert.eq(numShards, sendPrepareOp.length);

    const writingDecisionFilter = makeWorkerFilterWithAction(session, "writingDecision", txnNumber);
    let writeDecisionOp = curOpAfterFailpoint('hangBeforeWritingDecision', writingDecisionFilter);
    assert.eq(1, writeDecisionOp.length);

    const sendCommitFilter = makeWorkerFilterWithAction(session, "sendingCommit", txnNumber);
    let sendCommitOp = curOpAfterFailpoint('hangBeforeSendingCommit', sendCommitFilter, numShards);
    assert.eq(numShards, sendCommitOp.length);

    const deletingCoordinatorFilter =
        makeWorkerFilterWithAction(session, "deletingCoordinatorDoc", txnNumber);
    let deletingCoordinatorDocOp =
        curOpAfterFailpoint('hangBeforeDeletingCoordinatorDoc', deletingCoordinatorFilter);
    assert.eq(1, deletingCoordinatorDocOp.length);

    commitJoin();
}

jsTest.log("Testing that coordinator threads show up in currentOp for an abort decision.");
{
    let session = adminDB.getMongo().startSession();
    startTransaction(session, collectionName, 2);
    let txnNumber = session.getTxnNumber_forTesting();
    let lsid = session.getSessionId();
    // Manually abort the transaction on one of the participants, so that the participant fails to
    // prepare and failpoint is triggered on the coordinator.
    assert.commandWorked(participant.adminCommand({
        abortTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    }));
    let commitJoin = commitTxn(st, lsid, txnNumber, ErrorCodes.NoSuchTransaction);

    const sendAbortFilter = makeWorkerFilterWithAction(session, "sendingAbort", txnNumber);
    let sendingAbortOp = curOpAfterFailpoint('hangBeforeSendingAbort', sendAbortFilter, numShards);
    assert.eq(numShards, sendingAbortOp.length);

    commitJoin();
}

st.stop();
})();
