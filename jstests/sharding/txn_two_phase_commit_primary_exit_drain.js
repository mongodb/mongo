/**
 * Exercises the coordinator commands logic to test if a primary exits drain mode after the primary
 * steps down during a basic two phase commit.
 *
 * @tags: [uses_transactions, uses_prepare_transaction, uses_multi_shard_transaction,
 * multiversion_incompatible]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    checkDecisionIs,
    checkDocumentDeleted,
    runCommitThroughMongosInParallelThread
} from "jstests/sharding/libs/txn_two_phase_commit_util.js";

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

let st = new ShardingTest({shards: 3, causallyConsistent: true});

let coordinator = st.shard0;
let participant1 = st.shard1;
let participant2 = st.shard2;

let lsid = {id: UUID()};
let txnNumber = 0;

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

const testCommitProtocol = function() {
    jsTest.log("Testing two-phase commit");

    txnNumber++;
    setUp();

    const coordinatorPrimary = coordinator.rs.getPrimary();

    const hangBeforeWaitingForDecisionWriteConcernFp = configureFailPoint(
        coordinatorPrimary, "hangBeforeWaitingForDecisionWriteConcern", {}, "alwaysOn");

    let commitThread = runCommitThroughMongosInParallelThread(lsid, txnNumber, st.s.host);
    commitThread.start();

    // Check that the coordinator wrote the decision.
    hangBeforeWaitingForDecisionWriteConcernFp.wait();
    checkDecisionIs(coordinator, lsid, txnNumber, "commit");

    assert.commandWorked(
        coordinatorPrimary.adminCommand({replSetStepDown: ReplSetTest.kForeverSecs, force: true}));
    // The replSetFreeze command will cause the node to run for primary on its own.
    assert.commandWorked(coordinatorPrimary.adminCommand({replSetFreeze: 0}));

    // The primary won't be able to exit drain mode while the
    // hangBeforeWaitingForDecisionWriteConcern failpoint is active.
    coordinator.rs.waitForState(coordinatorPrimary, ReplSetTest.State.PRIMARY);
    const helloRes = assert.commandWorked(coordinatorPrimary.adminCommand("hello"));
    assert.eq(false, helloRes.isWritablePrimary, helloRes);
    hangBeforeWaitingForDecisionWriteConcernFp.off();
    // However after the hangBeforeWaitingForDecisionWriteConcern failpoint is disabled the primary
    // is expected to exit drain mode.

    commitThread.join();

    // Check that the coordinator deleted its persisted state.
    assert.soon(function() {
        return checkDocumentDeleted(coordinator, lsid, txnNumber);
    });

    // Check that the transaction committed as expected.

    jsTest.log("Verify that the transaction was committed on all shards.");
    // Use assert.soon(), because although coordinateCommitTransaction currently blocks
    // until the commit process is fully complete, it will eventually be changed to only
    // block until the decision is *written*, so the documents may not be visible
    // immediately.
    assert.soon(function() {
        return 3 === st.s.getDB(dbName).getCollection(collName).find().itcount();
    });
};

testCommitProtocol();

st.stop();
