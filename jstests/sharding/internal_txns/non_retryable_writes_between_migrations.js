/*
 * Test that non-retryable write internal transactions do not reset retryable write history in the
 * external session that they correspond to and cause a migration that moves a chunk back to a
 * shard to fail.
 *
 * @tags: [requires_fcv_60, uses_transactions, requires_persistence]
 */
import {
    withRetryOnTransientTxnErrorIncrementTxnNum
} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {makeCommitTransactionCmdObj} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const st = new ShardingTest({shards: 2});

const dbName = "testDb";
const collName = "testColl";
const ns = dbName + "." + collName;
const testDB = st.s.getDB(dbName);

// Set up a sharded collection with the following chunks:
// shard0: [MinKey, 0]
// shard1: [0, MaxKey]
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {x: 0}, to: st.shard1.shardName}));

const parentLsid = {
    id: UUID()
};
const parentTxnNumber = NumberLong(35);
// Perform a transaction against the chunk [MinKey, 0].
withRetryOnTransientTxnErrorIncrementTxnNum(parentTxnNumber, (txnNumber) => {
    assert.commandWorked(testDB.runCommand({
        insert: collName,
        documents: [{x: -1}],
        lsid: parentLsid,
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(
        testDB.adminCommand(makeCommitTransactionCmdObj(parentLsid, NumberLong(txnNumber))));
});

// Move the chunk [MinKey, 0] from shard0 to shard1 and then back to shard0.
assert.commandWorked(
    st.s.adminCommand({moveChunk: ns, find: {x: MinKey}, to: st.shard1.shardName}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: ns, find: {x: MinKey}, to: st.shard0.shardName}));

// Performs a non-retryable write against the chunk [0, MaxKey] in the same session. At this point
// shard1 has an active retryable write in that the session because of the dead-end sentinel noop
// oplog entry written during the previous incoming chunk migration.
const nonRetryableWriteChildLsid = {
    id: parentLsid.id,
    txnUUID: UUID()
};
const nonRetryableWriteTxnNumber = NumberLong(0);
withRetryOnTransientTxnErrorIncrementTxnNum(nonRetryableWriteTxnNumber, (txnNumber) => {
    assert.commandWorked(testDB.runCommand({
        insert: collName,
        documents: [{x: 1}],
        lsid: nonRetryableWriteChildLsid,
        txnNumber: NumberLong(txnNumber),
        startTransaction: true,
        autocommit: false
    }));
    assert.commandWorked(testDB.adminCommand(
        makeCommitTransactionCmdObj(nonRetryableWriteChildLsid, NumberLong(txnNumber))));
});

// Move the chunk [MinKey, 0] from shard0 and shard1. The migration should succeed.
assert.commandWorked(
    st.s.adminCommand({moveChunk: ns, find: {x: MinKey}, to: st.shard1.shardName}));

st.stop();
