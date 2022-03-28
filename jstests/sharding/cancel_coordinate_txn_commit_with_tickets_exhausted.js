/**
 * The test checks that the TransactionCoordinator will not crash if the transaction is aborted when
 * attempting to commit a transaction.
 *
 * Step 1. Run and commit a transaction in order to initialize TransactionCoordinator.
 *
 * Step 2. Run `kNumWriteTickets` remove operations in parallel. So that they take up all of the
 * WiredTiger tickets.
 *
 * Step 3. Run a transaction in parallel, but do not attempt to commit it until
 * all of the remove operations have taken WiredTiger tickets. Step 4. Wait for the transaction to
 * reach the `deletingCoordinatorDoc` state.
 *
 * Step 5. Turn off the `hangWithLockDuringBatchRemoveFp`
 * and join the parallel remove operations and transaction thread.
 *
 * @tags: [uses_multi_shard_transaction, uses_prepare_transaction]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallelTester.js');
load("jstests/sharding/libs/create_sharded_collection_util.js");

const kNumWriteTickets = 10;
const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    rs: {nodes: 1},
    rsOptions: {
        setParameter: {
            wiredTigerConcurrentWriteTransactions: kNumWriteTickets,
            // Lower transactionLifetimeLimitSeconds to cause TransactionCoordinators which haven't
            // yet made their commit or abort decision to time out and abort the transaction.
            transactionLifetimeLimitSeconds: 20,
        }
    }
});

const dbName = "test";
const collName = "mycoll";
const failpointName = 'hangWithLockDuringBatchRemove';
const sourceCollection = st.s.getDB(dbName).getCollection(collName);
const txnCoordinator = st.rs1.getPrimary();
const insertLatch = new CountDownLatch(1);

CreateShardedCollectionUtil.shardCollectionWithChunks(sourceCollection, {key: 1}, [
    {min: {key: MinKey}, max: {key: 0}, shard: st.shard0.shardName},
    {min: {key: 0}, max: {key: MaxKey}, shard: st.shard1.shardName},
]);

// The ShardServerCatalogCacheLoader thread may perform deletes in the background on the
// config.cache.chunks.test.mycoll collection. Its deletes are also eligible to be blocked by the
// hangWithLockDuringBatchRemove failpoint and will prevent a removeOperation thread from acquiring
// a WiredTiger write ticket. We wait for it to finish any delete operations by explicitly running
// the _flushRoutingTableCacheUpdates command.
assert.commandWorked(
    txnCoordinator.adminCommand({_flushRoutingTableCacheUpdates: sourceCollection.getFullName()}));

const removeOperationThreads = Array.from({length: kNumWriteTickets}).map(() => {
    return new Thread(function removeOperation(host, dbName, collName, insertLatch) {
        const conn = new Mongo(host);
        const testDB = conn.getDB(dbName);
        const coll = testDB.getCollection(collName);
        insertLatch.await();
        assert.commandWorked(coll.remove({key: 200}, {justOne: true}));
    }, st.s.host, dbName, collName, insertLatch);
});

const session = st.s.startSession({causalConsistency: false});
const sessionCollection = session.getDatabase(dbName).getCollection(collName);

// A two-phase commit transaction is first run to ensure the TransactionCoordinator has recovered
// and persisted a topology time. The transactionThread will run a second two-phase commit
// transaction using the same shard for coordinating the transaction. This ensures the
// transactionThread won't need to persist a topology time. The scenario reported in SERVER-60685
// depended on the TransactionCoordinator being interrupted while persisting the participant list
// which happens after waiting for the topology time to become durable.
session.startTransaction();
assert.commandWorked(sessionCollection.insert({key: 400}));
assert.commandWorked(sessionCollection.insert({key: -400}));
assert.commandWorked(session.commitTransaction_forTesting());

const hangWithLockDuringBatchRemoveFp = configureFailPoint(txnCoordinator, failpointName);

const transactionThread = new Thread(
    function runTwoPhaseCommitTxnAndTimeOutBeforeWritingParticipantList(
        host, dbName, collName, failpointName, totalWriteTickets, insertLatch) {
        const conn = new Mongo(host);
        const session = conn.startSession({causalConsistency: false});
        const sessionCollection = session.getDatabase(dbName).getCollection(collName);
        session.startTransaction();
        assert.commandWorked(sessionCollection.insert({key: 400}));
        assert.commandWorked(sessionCollection.insert({key: -400}));
        insertLatch.countDown();

        const currentOp = (pipeline = []) =>
            conn.getDB("admin").aggregate([{$currentOp: {}}, ...pipeline]).toArray();
        assert.soon(() => {
            const removeOperations = currentOp(
                [{$match: {op: "remove", msg: failpointName, ns: `${dbName}.${collName}`}}]);
            return removeOperations.length === totalWriteTickets;
        }, () => `Timed out waiting for the remove operations: ${tojson(currentOp())}`);

        // After here all of the WiredTiger write tickets should be taken.
        assert.commandFailedWithCode(
            session.commitTransaction_forTesting(),
            [ErrorCodes.TransactionCoordinatorReachedAbortDecision, ErrorCodes.NoSuchTransaction]);
    },
    st.s.host,
    dbName,
    collName,
    failpointName,
    kNumWriteTickets,
    insertLatch);

transactionThread.start();

removeOperationThreads.forEach(thread => thread.start());

transactionThread.join();
hangWithLockDuringBatchRemoveFp.off();

removeOperationThreads.forEach((thread) => {
    thread.join();
});

st.stop();
})();
