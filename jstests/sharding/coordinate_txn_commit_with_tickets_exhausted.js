/**
 * Validate SERVER-60682 and SERVER-92292: TransactionCoordinator won't starve for a storage ticket
 * to prepare or commit a cross-shard transaction.
 *
 * @tags: [
 *   requires_fcv_70,
 *   uses_transactions,
 *   uses_multi_shard_transaction,
 *   uses_prepare_transaction,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const kNumWriteTickets = 10;
const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    rs: {nodes: 1},
    rsOptions: {
        setParameter: {
            // This test requires a fixed ticket pool size.
            storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
            wiredTigerConcurrentWriteTransactions: kNumWriteTickets,
            // Raise maxTransactionLockRequestTimeoutMillis to prevent the transactions in prepare
            // conflict state from aborting early due to being unable to acquire a write ticket.
            // This is needed because we want to reproduce a scenario where the number of
            // transactions in prepare conflict state is greater or equal to the available storage
            // tickets.
            maxTransactionLockRequestTimeoutMillis: 24 * 60 * 60 * 1000,
            // Similarly, we need to keep transactions alive longer than the Evergreen test
            // execution timeout so as to be able to detect failure.
            // While the test environment may already set a large enough default
            // transactionLifetimeLimitSeconds, we nevertheless specify the lifetime to avoid
            // relying on a potentially changing default.
            transactionLifetimeLimitSeconds: 24 * 60 * 60,
        }
    }
});

const sourceCollection = st.s.getCollection("test.mycoll");
CreateShardedCollectionUtil.shardCollectionWithChunks(sourceCollection, {key: 1}, [
    {min: {key: MinKey}, max: {key: 0}, shard: st.shard0.shardName},
    {min: {key: 0}, max: {key: MaxKey}, shard: st.shard1.shardName},
]);

// Insert a document into each shard.
assert.commandWorked(sourceCollection.insert([{key: 200}, {key: -200}]));

const txnCoordinator = st.rs1.getPrimary();

// Create a thread which leaves the TransactionCoordinator in a state where prepareTransaction has
// been run on both participant shards and it is about to write the commit decision locally to the
// config.transaction_coordinators collection.
const hangBeforeWritingDecisionFp = configureFailPoint(txnCoordinator, "hangBeforeWritingDecision");
const preparedTxnThread = new Thread(function runTwoPhaseCommitTxn(host, dbName, collName) {
    const conn = new Mongo(host);
    const session = conn.startSession({causalConsistency: false});
    const sessionCollection = session.getDatabase(dbName).getCollection(collName);

    session.startTransaction();
    assert.commandWorked(sessionCollection.update({key: 200}, {$inc: {counter: 1}}));
    assert.commandWorked(sessionCollection.update({key: -200}, {$inc: {counter: 1}}));
    assert.commandWorked(session.commitTransaction_forTesting());
}, st.s.host, sourceCollection.getDB().getName(), sourceCollection.getName());
preparedTxnThread.start();
hangBeforeWritingDecisionFp.wait({timesEntered: 1});

// Create a thread which leaves the TransactionCoordinator in a state where write operations has
// been run on both participant shards and it is about to start the two-phase-commit protocol (e.g.
// before writing the participant list).
const hangBeforeWritingParticipantListFp =
    configureFailPoint(txnCoordinator, "hangBeforeWritingParticipantList");
const commitTxnThread = new Thread(function runTwoPhaseCommitTxn(host, dbName, collName) {
    const conn = new Mongo(host);
    const session = conn.startSession({causalConsistency: false});
    const sessionCollection = session.getDatabase(dbName).getCollection(collName);

    session.startTransaction();
    assert.commandWorked(sessionCollection.insert({key: 300}));
    assert.commandWorked(sessionCollection.insert({key: -300}));
    assert.commandWorked(session.commitTransaction_forTesting());
}, st.s.host, sourceCollection.getDB().getName(), sourceCollection.getName());
commitTxnThread.start();
hangBeforeWritingParticipantListFp.wait();

// Create other threads which will block on a prepare conflict while still holding a write ticket to
// test that the TransactionCoordinator from preparedTxnThread can still complete.
const prepareConflictThreads = [];
for (let i = 0; i < kNumWriteTickets; ++i) {
    const thread = new Thread(function hitPrepareConflictOnCoordinator(host, dbName, collName) {
        const conn = new Mongo(host);
        const session = conn.startSession({causalConsistency: false});
        const sessionCollection = session.getDatabase(dbName).getCollection(collName);

        session.startTransaction();
        // Do a write to ensure the transaction takes a write ticket.
        assert.commandWorked(sessionCollection.insert({key: 100}));
        // Then do a read which will block until the prepare conflict resolves.
        assert.eq({key: 200, counter: 1}, sessionCollection.findOne({key: 200}, {_id: 0}));
        assert.commandWorked(session.commitTransaction_forTesting());
    }, st.s.host, sourceCollection.getDB().getName(), sourceCollection.getName());
    prepareConflictThreads.push(thread);
    thread.start();
}

const currentOp = (pipeline = []) => st.admin.aggregate([{$currentOp: {}}, ...pipeline]).toArray();

assert.soon(() => {
    const ops = currentOp([{$match: {prepareReadConflicts: {$gt: 0}}}]);
    return ops.length >= Math.min(prepareConflictThreads.length, kNumWriteTickets);
}, () => `Failed to find prepare conflicts in $currentOp output: ${tojson(currentOp())}`);

// Allow the commitTxnThread to proceed with preparing the transaction. This tests that we skip
// write ticket acquisition when preparing a transaction.
hangBeforeWritingParticipantListFp.off();

jsTestLog("Waiting for commitTxnThread to successfully prepare the transaction");
hangBeforeWritingDecisionFp.wait({timesEntered: 3});

// Allow both prepared transactions to proceed with committing the transaction. This tests that we
// skip write ticket acquisition when persisting the transaction decision and committing the
// prepared transaction.
hangBeforeWritingDecisionFp.off();

jsTestLog("Waiting for both transactions to successfully commit");
preparedTxnThread.join();
commitTxnThread.join();

jsTestLog("Waiting for all transactional reads to complete");
for (let thread of prepareConflictThreads) {
    thread.join();
}

st.stop();
