/**
 * Tests that snapshot readConcern and placementConflictTime are respected on participants added to
 * a transaction by other participants when there are conflicting operations.
 * @tags: [requires_fcv_80]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

let st = new ShardingTest({shards: 2});

const dbName = "test";
const localColl = "local";
const foreignColl = "foreign";
const localNs = dbName + "." + localColl;
const foreignNs = dbName + "." + foreignColl;

let shard0 = st.shard0;
let shard1 = st.shard1;

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: shard0.shardName}));

// Create a collection, "local" that lives on shard0.
assert.commandWorked(st.s.getDB(dbName).local.insert({_id: 0, x: 1}));

// Create a sharded collection, "foreign", with the following chunks:
// shard0: [x: -inf, x: 0)
// shard1: [x: 0, x: +inf)
assert.commandWorked(st.s.adminCommand({shardCollection: foreignNs, key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: foreignNs, middle: {x: 0}}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: foreignNs, find: {x: -10}, to: shard0.shardName}));
assert.commandWorked(st.s.adminCommand({moveChunk: foreignNs, find: {x: 0}, to: shard1.shardName}));

// Force refreshes to avoid getting stale config errors
assert.commandWorked(shard0.adminCommand({_flushRoutingTableCacheUpdates: localNs}));
assert.commandWorked(shard1.adminCommand({_flushRoutingTableCacheUpdates: localNs}));
st.refreshCatalogCacheForNs(st.s, localNs);

assert.commandWorked(shard0.adminCommand({_flushRoutingTableCacheUpdates: foreignNs}));
assert.commandWorked(shard1.adminCommand({_flushRoutingTableCacheUpdates: foreignNs}));
st.refreshCatalogCacheForNs(st.s, foreignNs);

const session = st.s.startSession();
const sessionDB = session.getDatabase(dbName);

{
    print(
        "Testing that additional participant respects placementConflictTime with a conflicting migration");

    // Must use readConcern other than snapshot for txn to use placementConflictTime
    session.startTransaction({readConcern: {level: "majority"}});

    // Run a find that will target shard0
    assert.eq(sessionDB.getCollection(localColl).find().itcount(), 1);

    // Move the foreignColl chunk to shard1 from shard0 and refresh mongos
    assert.commandWorked(
        st.s.adminCommand({moveChunk: foreignNs, find: {x: -10}, to: shard1.shardName}));
    st.refreshCatalogCacheForNs(st.s, foreignNs);

    // Run a $lookup which will add shard1 as an additional participant. This should throw
    // because shard1 had an incoming migration.
    let err = assert.throwsWithCode(() => {
        sessionDB.getCollection(localColl).aggregate(
            [{$lookup: {from: foreignColl, localField: "x", foreignField: "_id", as: "result"}}]);
    }, ErrorCodes.MigrationConflict);
    assert.contains("TransientTransactionError", err.errorLabels, tojson(err));

    session.abortTransaction();
}

{
    print(
        "Testing that additional participants respects readConcern snapshot with a conflicting write");

    // Insert a doc in the foreign collection that we will later update
    assert.commandWorked(st.s.getDB(dbName).foreign.insert({_id: 1, x: 1}));

    // Start a transaction on shard0
    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.eq(sessionDB.getCollection(localColl).find().itcount(), 1);

    // Update the doc in foreignColl in a different transaction
    const session2 = st.s.startSession();
    const sessionDB2 = session2.getDatabase(dbName);
    session2.startTransaction();
    assert.commandWorked(sessionDB2.getCollection(foreignColl).update({x: 1}, {$set: {x: 2}}));
    assert.commandWorked(session2.commitTransaction_forTesting());

    // Run a $lookup that will add shard1 as an additional participant in the first transaction
    // and ensure it does not see the updated value, meaning that it returns a matching doc
    let aggCmd = [{$lookup: {from: foreignColl, localField: "x", foreignField: "x", as: "result"}}];

    const expectedTxnAggRes = [{_id: 0, x: 1, result: [{_id: 1, x: 1}]}];
    let txnAggRes =
        sessionDB.getCollection(localColl).aggregate(aggCmd, {cursor: {batchSize: 2}}).toArray();
    assertArrayEq({actual: txnAggRes, expected: expectedTxnAggRes});

    // For comparison, run $lookup outside of a transaction and assert we see the updated
    // value, meaning that it does not return a matching doc
    const expectedNonTxnAggRes = [{_id: 0, x: 1, result: []}];
    let nonTxnAggRes =
        st.s.getDB(dbName).local.aggregate(aggCmd, {cursor: {batchSize: 2}}).toArray();
    assertArrayEq({actual: nonTxnAggRes, expected: expectedNonTxnAggRes});

    assert.commandWorked(session.commitTransaction_forTesting());
}

st.stop();
