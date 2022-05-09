/**
 * Tests running the setFeatureCompatibilityVersion command concurrently with a prepared
 * transaction. Specifically, runs the setFCV right before the TransactionCoordinator writes the
 * commit decision.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const st = new ShardingTest({shards: 2});
const shard0Primary = st.rs0.getPrimary();

// Set up a sharded collection with two chunks:
// shard0: [MinKey, 0]
// shard1: [0, MaxKey]
const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: ns, find: {x: MinKey}, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 1}, to: st.shard1.shardName}));

function runTxn(mongosHost, dbName, collName) {
    const mongosConn = new Mongo(mongosHost);
    jsTest.log("Starting a cross-shard transaction with shard0 and shard1 as the participants " +
               "and shard0 as the coordinator shard");
    const lsid = {id: UUID()};
    const txnNumber = NumberLong(35);
    assert.commandWorked(mongosConn.getDB(dbName).runCommand({
        insert: collName,
        documents: [{x: -1}],
        lsid,
        txnNumber,
        startTransaction: true,
        autocommit: false,
    }));
    assert.commandWorked(mongosConn.getDB(dbName).runCommand({
        insert: collName,
        documents: [{x: 1}],
        lsid,
        txnNumber,
        autocommit: false,
    }));
    assert.commandWorked(
        mongosConn.adminCommand({commitTransaction: 1, lsid, txnNumber, autocommit: false}));
    jsTest.log("Committed the cross-shard transaction");
}

function runSetFCV(primaryHost) {
    const primaryConn = new Mongo(primaryHost);
    jsTest.log("Starting a setFCV command on " + primaryHost);
    assert.commandWorked(primaryConn.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    jsTest.log("Completed the setFCV command");
}

// Run a cross-shard transaction that has shard0 as the coordinator. Make the TransactionCoordinator
// thread hang right before the commit decision is written (i.e. after the transaction has entered
// the "prepared" state).
const writeDecisionFp = configureFailPoint(shard0Primary, "hangBeforeWritingDecision");
const txnThread = new Thread(runTxn, st.s.host, dbName, collName);
txnThread.start();
writeDecisionFp.wait();

// Run a setFCV command against shard0 and wait for the setFCV thread to start waiting to acquire
// the setFCV S lock (i.e. waiting for existing prepared transactions to commit or abort).
const setFCVThread = new Thread(runSetFCV, shard0Primary.host);
setFCVThread.start();
assert.soon(() => {
    return shard0Primary.getDB(dbName).currentOp().inprog.find(
        op => op.command && op.command.setFeatureCompatibilityVersion && op.locks &&
            op.locks.FeatureCompatibilityVersion === "R" && op.waitingForLock === true);
});

// Unpause the TransactionCoordinator. The transaction should be able to commit despite the fact
// that the FCV S lock is enqueued.
writeDecisionFp.off();

jsTest.log("Waiting for the cross-shard transaction to commit");
txnThread.join();
jsTest.log("Waiting for setFCV command to complete");
setFCVThread.join();
jsTest.log("Done");

st.stop();
})();
