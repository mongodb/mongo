/**
 * Sanity checks that illegally attempting to use more than one router for the
 * same transaction does not leave the server in an invalid state.
 *
 * @tags: [uses_transactions, uses_prepare_transaction, uses_multi_shard_transaction]
 */

(function() {
'use strict';

load('jstests/sharding/libs/sharded_transactions_helpers.js');

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

function removeAllDocumentsFromTestCollection() {
    assert.commandWorked(router0.getDB(dbName).foo.deleteMany({}));
}

function runTest(testFn) {
    testFn();
    removeAllDocumentsFromTestCollection();
}

let st = new ShardingTest({shards: 3, mongos: 2, causallyConsistent: true});
let router0 = st.s0;
let router1 = st.s1;

// Create a sharded collection with a chunk on each shard:
// shard0: [-inf, 0)
// shard1: [0, 10)
// shard2: [10, +inf)
assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 10}}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 10}, to: st.shard2.shardName}));

flushRoutersAndRefreshShardMetadata(st, {ns});

// Test that trying to run start txn from two routers with the same transaction number fails
// through the second router if they target overlapping shards.
runTest(() => {
    let lsid = {id: UUID()};
    let txnNumber = 0;

    // Start a new transaction on router 0 by inserting a document onto each shard.
    assert.commandWorked(router0.getDB(dbName).runCommand({
        insert: collName,
        documents: [{_id: -5}, {_id: 5}, {_id: 15}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
    }));

    // Try to start a new transaction with the same transaction number on router 1 by inserting
    // a document onto each shard.
    assert.commandFailedWithCode(router1.getDB(dbName).runCommand({
        insert: collName,
        documents: [{_id: -50}, {_id: 4}, {_id: 150}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        startTransaction: true,
        autocommit: false,
        // Because ordered writes are done serially for different shard targets and abort early
        // on first error, this can leave the transaction on the other shards open.
        // To ensure this router implicitly aborts the transaction on all participants (so
        // that the next test case does not encounter an open transaction), make this
        // router do an *unordered* write that touches all the same participants as the
        // first router touched.
        ordered: false,
    }),
                                 50911);

    // The automatic abort-on-error path will occur when the above
    // statement fails, so commit will fail.
    assert.commandFailedWithCode(router0.getDB(dbName).adminCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    }),
                                 ErrorCodes.NoSuchTransaction);
});

// Test that trying to run start txn from one router and running an operation for that same
// transaction from another router fails through the second router.
runTest(() => {
    let lsid = {id: UUID()};
    let txnNumber = 0;
    let stmtId = 0;

    // Start a new transaction on router 0 by inserting a document onto each shard.
    assert.commandWorked(router0.getDB(dbName).runCommand({
        insert: collName,
        documents: [{_id: -5}, {_id: 5}, {_id: 15}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId),
        startTransaction: true,
        autocommit: false,
    }));

    ++stmtId;

    // Try to continue the same transaction but through router 1. Should fail because no txn
    // with this number exists on that router.
    assert.commandFailed(router1.getDB(dbName).runCommand({
        insert: collName,
        documents: [{_id: -50}, {_id: 4}, {_id: 150}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId),
        autocommit: false,
    }));

    // Commit should succeed since the command from router 2 should never reach the shard.
    assert.commandWorked(router0.getDB(dbName).adminCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(0),
        autocommit: false,
    }));
});

// Test that trying to run start txn from one router, start txn on the second router with the
// same transaction number, and running operations on overlapping shards will lead to failure.
runTest(() => {
    let lsid = {id: UUID()};
    let txnNumber = 0;
    let stmtId = 0;

    // Start a new transaction on router 0 by inserting a document onto the first shard
    assert.commandWorked(router0.getDB(dbName).runCommand({
        insert: collName,
        documents: [{_id: -5}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId),
        startTransaction: true,
        autocommit: false,
    }));

    // Start a new transaction on router 1 with the same transaction number, targeting the last
    // shard.
    assert.commandWorked(router1.getDB(dbName).runCommand({
        insert: collName,
        documents: [{_id: 15}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId),
        startTransaction: true,
        autocommit: false,
    }));

    ++stmtId;

    // Try to do an operation on the last shard through router 0. Fails because it sends
    // startTxn: true to the new participant, which has already seen an operation from router 1.
    // Implicitly aborts the transaction when the error is thrown.
    assert.commandFailedWithCode(router0.getDB(dbName).runCommand({
        insert: collName,
        documents: [{_id: 50}],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId),
        autocommit: false,
    }),
                                 50911);

    // Commit through router 0 should fail.
    assert.commandFailedWithCode(router0.getDB(dbName).adminCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId),
        autocommit: false,
    }),
                                 ErrorCodes.NoSuchTransaction);

    // Commit through router 1 should fail.
    assert.commandFailedWithCode(router1.getDB(dbName).adminCommand({
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        stmtId: NumberInt(stmtId),
        autocommit: false,
    }),
                                 ErrorCodes.NoSuchTransaction);
});

st.stop();
})();
