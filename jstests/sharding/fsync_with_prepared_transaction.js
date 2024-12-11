/*
This test runs a cross-shard transaction where the transaction is in the prepared state, then run
an fsyncLock which will acquire the global S lock once the prepared transaction commits.
 * @tags: [
 *   requires_sharding,
 *   requires_fsync,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    config: 1,
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

function waitForFsyncLockToWaitForLock(st, numThreads) {
    assert.soon(() => {
        let ops = st.s.getDB('admin')
                      .aggregate([
                          {$currentOp: {allUsers: true, idleConnections: true}},
                          {$match: {desc: "fsyncLockWorker", waitingForLock: true}},
                      ])
                      .toArray();
        if (ops.length != numThreads) {
            jsTest.log("Num operations: " + ops.length + ", expected: " + numThreads);
            jsTest.log(ops);
            return false;
        }
        return true;
    });
}

let runTxn = async function(mongosHost, dbName, collName) {
    const {withTxnAndAutoRetryOnMongos} =
        await import("jstests/libs/auto_retry_transaction_in_sharding.js");

    const mongosConn = new Mongo(mongosHost);
    jsTest.log("Starting a cross-shard transaction with shard0 and shard1 as the participants " +
               "and shard0 as the coordinator shard");

    let session = mongosConn.startSession();
    withTxnAndAutoRetryOnMongos(session, () => {
        assert.commandWorked(session.getDatabase(dbName).runCommand({
            insert: collName,
            documents: [{x: -1}],
        }));

        assert.commandWorked(session.getDatabase(dbName).runCommand({
            insert: collName,
            documents: [{x: 1}],
        }));
    });
    jsTest.log("Committed the cross-shard transaction");
};

function runFsyncLock(primaryHost) {
    let primaryConn = new Mongo(primaryHost);
    assert.commandWorked(primaryConn.adminCommand({fsync: 1, lock: true}));
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

// Wait for fsyncLockWorker threads on the shard primaries to wait for the global S lock (enqueued
// in the conflict queue).
waitForFsyncLockToWaitForLock(st, 2 /*blocked fsyncLockWorker threads*/);

// Unpause the TransactionCoordinator.
writeDecisionFp.off();

// fsyncLock completes, because the prepared transaction has committed.
fsyncLockThread.join();

// fsyncUnlock to allow the committed prepared transaction to return to the client. Whilst the
// transaction has committed, majority acknowledgement is still queued behind the global S lock;
// fsyncUnlock rescinds the global S lock, which in turn allows the transaction to be majority
// committed and the client to return an OK response.
assert.commandWorked(st.s.adminCommand({fsyncUnlock: 1}));
txnThread.join();
st.stop();
