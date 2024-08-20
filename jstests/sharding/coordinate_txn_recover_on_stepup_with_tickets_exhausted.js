/**
 * Validate that the TransactionCoordinator for a prepared transaction can be recovered on step-up
 * and commit the transaction when there are no storage tickets available. See SERVER-82883 and
 * SERVER-60682.
 *
 * @tags: [
 *   requires_fcv_70,
 *   uses_transactions,
 *   uses_multi_shard_transaction,
 *   uses_prepare_transaction,
 *   # The hangBeforeWritingDecision failpoint does not exist on older versions of mongodb.
 *   multiversion_incompatible,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";

const st = new ShardingTest({
    mongos: 1,
    config: 1,
    shards: 2,
    rs: {nodes: 3},
    rsOptions: {
        setParameter: {
            storageEngineConcurrencyAdjustmentAlgorithm: "fixedConcurrentTransactions",
            maxTransactionLockRequestTimeoutMillis: 24 * 60 * 60 * 1000,
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

// Create a thread which leaves the TransactionCoordinator in a state where
// prepareTransaction has been run on both participant shards and it is about to write
// the commit decision locally to the config.transaction_coordinators collection.
const preparedTxnThread = new Thread(function runTwoPhaseCommitTxn(host, dbName, collName) {
    const conn = new Mongo(host);
    const session = conn.startSession({causalConsistency: false});
    const sessionCollection = session.getDatabase(dbName).getCollection(collName);

    session.startTransaction();
    assert.commandWorked(sessionCollection.update({key: 200}, {$inc: {counter: 1}}));
    assert.commandWorked(sessionCollection.update({key: -200}, {$inc: {counter: 1}}));
    assert.commandWorked(session.commitTransaction_forTesting());
}, st.s.host, sourceCollection.getDB().getName(), sourceCollection.getName());
const txnCoordinator = st.rs1.getPrimary();
const hangBeforeWritingDecisionFp = configureFailPoint(txnCoordinator, "hangBeforeWritingDecision");

preparedTxnThread.start();
hangBeforeWritingDecisionFp.wait();

// Step-up the secondary and make it hang before doing the work to recover
// the TransactionCoordinator for the prepared transaction on step-up.
const secondary = st.rs1.getSecondary();
const hangBeforeTxnCoordinatorOnStepUpWorkFp =
    configureFailPoint(secondary, "hangBeforeTxnCoordinatorOnStepUpWork");
st.rs1.stepUp(secondary, {awaitWritablePrimary: false, awaitReplicationBeforeStepUp: false});
hangBeforeTxnCoordinatorOnStepUpWorkFp.wait();

const hangBeforeWritingEndOfTransactionFp =
    configureFailPoint(secondary, "hangBeforeWritingEndOfTransaction");

// Set the read and write tickets to 0 before executing the code to recover the
// TransactionCoordinator.
assert.commandWorked(secondary.getDB("admin").adminCommand(
    {setParameter: 1, wiredTigerConcurrentReadTransactions: 0}));
assert.commandWorked(secondary.getDB("admin").adminCommand(
    {setParameter: 1, wiredTigerConcurrentWriteTransactions: 0}));
hangBeforeTxnCoordinatorOnStepUpWorkFp.off();

// The TransactionCoordinator has successfully been recovered and the prepared transaction has been
// committed once this failpoint has been reached.
hangBeforeWritingEndOfTransactionFp.wait();
hangBeforeWritingEndOfTransactionFp.off();

// Reset the read and write tickets to a non-zero value to allow the test to finish.
assert.commandWorked(secondary.getDB("admin").adminCommand(
    {setParameter: 1, wiredTigerConcurrentReadTransactions: 128}));
assert.commandWorked(secondary.getDB("admin").adminCommand(
    {setParameter: 1, wiredTigerConcurrentWriteTransactions: 128}));

preparedTxnThread.join();
st.stop();
