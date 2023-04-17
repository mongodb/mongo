/**
 * Tests that additional participants can be added to an existing transaction when the
 * 'featureFlagAdditionalParticipants' is enabled.
 */

(function() {
'use strict';

load("jstests/libs/feature_flag_util.js");  // for FeatureFlagUtil.isEnabled
load("jstests/libs/fail_point_util.js");
load('jstests/sharding/libs/sharded_transactions_helpers.js');

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

const shard0Name = TestData.configShard ? "config" : "txn_addingParticipantParameter-rs0";
const shard1Name = "txn_addingParticipantParameter-rs1";
const shard2Name = "txn_addingParticipantParameter-rs2";
const shard3Name = "txn_addingParticipantParameter-rs3";

const checkParticipantListMatches = function(
    coordinatorConn, lsid, txnNumber, expectedParticipantList) {
    let coordDoc = coordinatorConn.getDB("config")
                       .getCollection("transaction_coordinators")
                       .findOne({"_id.lsid.id": lsid.id, "_id.txnNumber": txnNumber});
    assert.neq(null, coordDoc);
    assert.sameMembers(coordDoc.participants, expectedParticipantList);
};

const runCommitThroughMongosInParallelShellExpectAbort = function(st, lsid) {
    const runCommitExpectAbortCode = "assert.commandFailedWithCode(db.adminCommand({" +
        "commitTransaction: 1," +
        "lsid: " + tojson(lsid) + "," +
        "txnNumber: NumberLong(1)," +
        "stmtId: NumberInt(0)," +
        "autocommit: false," +
        "})," +
        "ErrorCodes.NoSuchTransaction);";
    return startParallelShell(runCommitExpectAbortCode, st.s.port);
};

const runCommitThroughMongosInParallelShellExpectSuccess = function(st, lsid) {
    const runCommitExpectSuccessCode = "assert.commandWorked(db.adminCommand({" +
        "commitTransaction: 1," +
        "lsid: " + tojson(lsid) + "," +
        "txnNumber: NumberLong(1)," +
        "stmtId: NumberInt(0)," +
        "autocommit: false," +
        "}));";
    return startParallelShell(runCommitExpectSuccessCode, st.s.port);
};

const testAddingParticipant = function(turnFailPointOn, expectedParticipantList, fpData = {}) {
    let st = new ShardingTest({shards: 4, causallyConsistent: true});

    // SERVER-67748
    const featureFlagAdditionalParticipants = FeatureFlagUtil.isEnabled(
        st.configRS.getPrimary().getDB('admin'), "AdditionalParticipants");
    if (!featureFlagAdditionalParticipants) {
        jsTestLog("Skipping as featureFlagAdditionalParticipants is  not enabled");
        st.stop();
        return;
    }

    let coordinator = st.shard0;
    let participant1 = st.shard1;
    let shard2 = st.shard2;

    let lsid = {id: UUID()};

    const session = st.s.startSession();
    const sessionDB = session.getDatabase(dbName);

    if (turnFailPointOn) {
        // Turn on failpoint so that the shard1 writes the participant paramenter in its
        // response body to transaction router.
        configureFailPoint(
            participant1, "includeAdditionalParticipantInResponse", fpData, "alwaysOn");
    }

    // Create a sharded collection with a chunk on each shard:
    // shard0: [-inf, 0)
    // shard1: [0, 10)
    // shard2: [10, +inf)
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: coordinator.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: shard2.shardName}));

    // These forced refreshes are not strictly necessary; they just prevent extra TXN log lines
    // from the shards starting, aborting, and restarting the transaction due to needing to
    // refresh after the transaction has started.
    assert.commandWorked(coordinator.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    assert.commandWorked(participant1.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    assert.commandWorked(shard2.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    st.refreshCatalogCacheForNs(st.s, ns);

    // Start a new transaction by inserting a document onto shard0 and shard1.
    assert.commandWorked(sessionDB.runCommand({
        insert: collName,
        documents: [{_id: -5}, {_id: 5}],
        lsid: lsid,
        txnNumber: NumberLong(1),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }));

    let awaitResult;
    if (turnFailPointOn) {
        // Since feature flag was enabled, we expect shard2 was added to the list of participants
        // stored on the coordinator and the commit to fail because of this new participant.
        awaitResult = runCommitThroughMongosInParallelShellExpectAbort(st, lsid);
    } else {
        awaitResult = runCommitThroughMongosInParallelShellExpectSuccess(st, lsid);
    }

    // Turn on failpoints so that the coordinator hangs after each write it does, so that the
    // test can check that the write happened correctly.
    const hangBeforeWaitingForParticipantListWriteConcernFp = configureFailPoint(
        coordinator, "hangBeforeWaitingForParticipantListWriteConcern", {}, "alwaysOn");

    // Check that the coordinator wrote the participant list.
    hangBeforeWaitingForParticipantListWriteConcernFp.wait();
    checkParticipantListMatches(coordinator, lsid, 1, expectedParticipantList);
    hangBeforeWaitingForParticipantListWriteConcernFp.off();

    awaitResult();

    if (turnFailPointOn) {
        // Check that the transaction aborted as expected.
        jsTest.log("Verify that the transaction was aborted on all shards.");
        assert.eq(0, st.s.getDB(dbName).getCollection(collName).find().itcount());
    }

    st.s.getDB(dbName).getCollection(collName).drop();
    st.stop();
};

jsTestLog("===Additional Participants Fail Point is OFF===");

let expectedParticipantListNormal = [shard0Name, shard1Name];
testAddingParticipant(false, expectedParticipantListNormal);

jsTestLog("===Additional Participants Fail Point is ON===");

print("Adding one additional participant:");
const fpDataOne = {
    "cmdName": "insert",
    "ns": ns,
    "shardId": ["txn_addingParticipantParameter-rs2"]
};
let expectedParticipantListOne = [shard0Name, shard1Name, shard2Name];
testAddingParticipant(true, expectedParticipantListOne, fpDataOne);

print("Adding multiple additional participants:");
const fpDataMultiple = {
    "cmdName": "insert",
    "ns": ns,
    "shardId": ["txn_addingParticipantParameter-rs2", "txn_addingParticipantParameter-rs3"]
};
let expectedParticipantListMultiple = [shard0Name, shard1Name, shard2Name, shard3Name];
testAddingParticipant(true, expectedParticipantListMultiple, fpDataMultiple);
})();
