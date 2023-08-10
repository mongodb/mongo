/*
This test runs a cross-shard transaction where the transaction holds onto the collection locks, then
runs an fsyncLock which should fail and timeout as the global S lock cannot be taken.
 * @tags: [
 *   requires_sharding,
 *   requires_fsync,
 *   featureFlagClusterFsyncLock
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

(function() {
'use strict';

load("jstests/libs/parallelTester.js");

const st = new ShardingTest({
    shards: 2,
    shardOptions: {setParameter: {featureFlagClusterFsyncLock: true}},
    mongos: 1,
    mongosOptions: {setParameter: {featureFlagClusterFsyncLock: true}},
    config: 1,
    configOptions: {setParameter: {featureFlagClusterFsyncLock: true}},
});
const shard0Primary = st.rs0.getPrimary();

// Set up a sharded collection with two chunks
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
    jsTest.log("Committing the cross-shard transaction");
    assert.commandWorked(
        mongosConn.adminCommand({commitTransaction: 1, lsid, txnNumber, autocommit: false}));
    jsTest.log("Committed the cross-shard transaction");
}

function runFsyncLock(primaryHost) {
    let primaryConn = new Mongo(primaryHost);
    let ret = assert.commandFailed(
        primaryConn.adminCommand({fsync: 1, lock: true, fsyncLockAcquisitionTimeoutMillis: 5000}));
    let errmsg = "Fsync lock timed out";
    assert.eq(ret.errmsg.includes(errmsg), true);
}

// Run a cross-shard transaction that has shard0 as the coordinator. Make the TransactionCoordinator
// thread hang right before the commit decision is written (i.e. after the transaction has entered
// the "prepared" state).
// This way the txn thread holds onto the collection locks
let writeDecisionFp = configureFailPoint(shard0Primary, "hangBeforeWritingDecision");
let txnThread = new Thread(runTxn, st.s.host, dbName, collName);
txnThread.start();
writeDecisionFp.wait();

let fsyncLockThread = new Thread(runFsyncLock, st.s.host);
fsyncLockThread.start();

// Wait for fsyncLock to wait for the global S lock, may have to be changed to a failpoint in the
// future (sleep is not as deterministic)
sleep(100);

// Unpause the TransactionCoordinator.
// The transaction thread can now acquire the IX locks since the blocking fsyncLock request (with an
// incompatible Global S lock) has been removed from the conflict queue
writeDecisionFp.off();

fsyncLockThread.join();

txnThread.join();

st.stop();
})();
