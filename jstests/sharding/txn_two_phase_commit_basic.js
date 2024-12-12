/**
 * Exercises the coordinator commands logic by simulating a basic two phase commit and basic two
 * phase abort.
 *
 * @tags: [uses_transactions, uses_prepare_transaction, uses_multi_shard_transaction]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    checkDecisionIs,
    checkDocumentDeleted,
    runCommitThroughMongosInParallelThread
} from 'jstests/sharding/libs/txn_two_phase_commit_util.js';

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

let st = new ShardingTest({shards: 3, causallyConsistent: true});

let coordinator = st.shard0;
let participant1 = st.shard1;
let participant2 = st.shard2;

let expectedParticipantList =
    [participant1.shardName, participant2.shardName, coordinator.shardName];

let lsid = {id: UUID()};
let txnNumber = 0;

const checkParticipantListMatches = function(
    coordinatorConn, lsid, txnNumber, expectedParticipantList) {
    let coordDoc = coordinatorConn.getDB("config")
                       .getCollection("transaction_coordinators")
                       .findOne({"_id.lsid.id": lsid.id, "_id.txnNumber": txnNumber});
    assert.neq(null, coordDoc);
    assert.sameMembers(coordDoc.participants, expectedParticipantList);
};

const startSimulatingNetworkFailures = function(connArray) {
    connArray.forEach(function(conn) {
        assert.commandWorked(conn.adminCommand({
            configureFailPoint: "failCommand",
            mode: {times: 10},
            data: {
                errorCode: ErrorCodes.NotWritablePrimary,
                failCommands: ["prepareTransaction", "abortTransaction", "commitTransaction"]
            }
        }));
        assert.commandWorked(conn.adminCommand({
            configureFailPoint: "participantReturnNetworkErrorForPrepareAfterExecutingPrepareLogic",
            mode: {times: 5}
        }));
        assert.commandWorked(conn.adminCommand({
            configureFailPoint: "participantReturnNetworkErrorForAbortAfterExecutingAbortLogic",
            mode: {times: 5}
        }));
        assert.commandWorked(conn.adminCommand({
            configureFailPoint: "participantReturnNetworkErrorForCommitAfterExecutingCommitLogic",
            mode: {times: 5}
        }));
    });
};

const stopSimulatingNetworkFailures = function(connArray) {
    connArray.forEach(function(conn) {
        assert.commandWorked(conn.adminCommand({
            configureFailPoint: "failCommand",
            mode: "off",
        }));
        assert.commandWorked(conn.adminCommand({
            configureFailPoint: "participantReturnNetworkErrorForPrepareAfterExecutingPrepareLogic",
            mode: "off"
        }));
        assert.commandWorked(conn.adminCommand({
            configureFailPoint: "participantReturnNetworkErrorForAbortAfterExecutingAbortLogic",
            mode: "off"
        }));
        assert.commandWorked(conn.adminCommand({
            configureFailPoint: "participantReturnNetworkErrorForCommitAfterExecutingCommitLogic",
            mode: "off"
        }));
    });
};

const setUp = function() {
    // Create a sharded collection with a chunk on each shard:
    // shard0: [-inf, 0)
    // shard1: [0, 10)
    // shard2: [10, +inf)
    assert.commandWorked(
        st.s.adminCommand({enableSharding: dbName, primaryShard: coordinator.shardName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: participant1.shardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: participant2.shardName}));

    // These forced refreshes are not strictly necessary; they just prevent extra TXN log lines
    // from the shards starting, aborting, and restarting the transaction due to needing to
    // refresh after the transaction has started.
    assert.commandWorked(coordinator.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    assert.commandWorked(participant1.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    assert.commandWorked(participant2.adminCommand({_flushRoutingTableCacheUpdates: ns}));
    st.refreshCatalogCacheForNs(st.s, ns);

    // Start a new transaction by inserting a document onto each shard.
    assert.commandWorked(st.s.getDB(dbName).runCommand({
        insert: collName,
        documents: [{_id: -5}, {_id: 5}, {_id: 15}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }));
};

const testCommitProtocol = function(shouldCommit, simulateNetworkFailures) {
    jsTest.log("Testing two-phase " + (shouldCommit ? "commit" : "abort") +
               " protocol with simulateNetworkFailures: " + simulateNetworkFailures);

    txnNumber++;
    setUp();

    if (!shouldCommit) {
        // Manually abort the transaction on one of the participants, so that the participant
        // fails to prepare.
        assert.commandWorked(participant2.adminCommand({
            abortTransaction: 1,
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            stmtId: NumberInt(0),
            autocommit: false,
        }));
    }

    if (simulateNetworkFailures) {
        startSimulatingNetworkFailures([participant1, participant2]);
    }

    // Turn on failpoints so that the coordinator hangs after each write it does, so that the
    // test can check that the write happened correctly.
    const hangBeforeWaitingForParticipantListWriteConcernFp = configureFailPoint(
        coordinator, "hangBeforeWaitingForParticipantListWriteConcern", {}, "alwaysOn");

    const hangBeforeWaitingForDecisionWriteConcernFp =
        configureFailPoint(coordinator, "hangBeforeWaitingForDecisionWriteConcern", {}, "alwaysOn");

    // Run commitTransaction through a parallel shell.
    let commitThread;
    if (shouldCommit) {
        commitThread = runCommitThroughMongosInParallelThread(lsid, txnNumber, st.s.host);
    } else {
        commitThread = runCommitThroughMongosInParallelThread(
            lsid, txnNumber, st.s.host, ErrorCodes.NoSuchTransaction);
    }
    commitThread.start();

    // Check that the coordinator wrote the participant list.
    hangBeforeWaitingForParticipantListWriteConcernFp.wait();
    checkParticipantListMatches(coordinator, lsid, txnNumber, expectedParticipantList);
    hangBeforeWaitingForParticipantListWriteConcernFp.off();

    // Check that the coordinator wrote the decision.
    hangBeforeWaitingForDecisionWriteConcernFp.wait();
    checkParticipantListMatches(coordinator, lsid, txnNumber, expectedParticipantList);
    checkDecisionIs(coordinator, lsid, txnNumber, (shouldCommit ? "commit" : "abort"));
    hangBeforeWaitingForDecisionWriteConcernFp.off();

    // Check that the coordinator deleted its persisted state.
    commitThread.join();
    assert.soon(function() {
        return checkDocumentDeleted(coordinator, lsid, txnNumber);
    });

    if (simulateNetworkFailures) {
        stopSimulatingNetworkFailures([participant1, participant2]);
    }

    // Check that the transaction committed or aborted as expected.
    if (!shouldCommit) {
        jsTest.log("Verify that the transaction was aborted on all shards.");
        assert.eq(0, st.s.getDB(dbName).getCollection(collName).find().itcount());
    } else {
        jsTest.log("Verify that the transaction was committed on all shards.");
        // Use assert.soon(), because although coordinateCommitTransaction currently blocks
        // until the commit process is fully complete, it will eventually be changed to only
        // block until the decision is *written*, so the documents may not be visible
        // immediately.
        assert.soon(function() {
            return 3 === st.s.getDB(dbName).getCollection(collName).find().itcount();
        });
    }

    st.s.getDB(dbName).getCollection(collName).drop();
};

testCommitProtocol(false /* test abort */, false /* no network failures */);
testCommitProtocol(true /* test commit */, false /* no network failures */);
testCommitProtocol(false /* test abort */, true /* with network failures */);
testCommitProtocol(true /* test commit */, true /* with network failures */);

st.stop();
