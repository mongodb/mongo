/**
 * Ensure that the query plan cache will not block the removal of orphaned documents.
 *
 * @tags: [
 *   # This test requires the fix from SERVER-73032.
 *   requires_fcv_63,
 *   # SBE is not yet used for clustered collections, and this test centers on the behavior of the
 *   # SBE plan cache.
 *   expects_explicit_underscore_id_index,
 * ]
 */
import {getPlanCacheKeyFromShape} from "jstests/libs/analyze_plan.js";
import {checkSBEEnabled} from "jstests/libs/sbe_util.js";

const dbName = "test";
const collName = "sbe_plan_cache_does_not_block_range_deletion";
const ns = dbName + "." + collName;

const st = new ShardingTest({mongos: 1, config: 1, shards: 2});

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardname}));

const isSBEEnabled = checkSBEEnabled(st.s.getDB(dbName));

const coll = st.s.getDB(dbName)[collName];

// Runs a test case against 'coll' after setting it up to have the given list of 'indexes' and a
// single 'document'. The test will execute a simple find command with the predicate 'filter'. Then
// it makes sure that the find command results in a cached plan and verifies that the existence of
// the cached plan does not interfere with range deletion.
function runTest({indexes, document, filter}) {
    coll.drop();
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));

    for (let index of indexes) {
        assert.commandWorked(coll.createIndex(index));
    }

    assert.commandWorked(coll.insert(document));

    // Run the same query twice to create an active plan cache entry.
    for (let i = 0; i < 2; ++i) {
        assert.eq(1, coll.find(filter).itcount());
    }

    // Ensure there is a cache entry we just created in the plan cache.
    const keyHash =
        getPlanCacheKeyFromShape({query: filter, collection: coll, db: st.s.getDB(dbName)});
    const res =
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: keyHash}}]).toArray();
    assert.eq(1, res.length);

    // Move the chunk to the second shard leaving orphaned documents on the first shard.
    assert.commandWorked(st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.name}));

    assert.soon(() => {
        // Ensure that the orphaned documents can be deleted.
        //
        // The "rangeDeletions" collection exists on each shard and stores a document for each chunk
        // range that contains orphaned documents. When the orphaned chunk range is cleaned up, the
        // document describing the range is deleted from the collection.
        return st.shard0.getDB('config')["rangeDeletions"].find().itcount() === 0;
    });
}

// Scenario with just one available indexed plan. If SBE is enabled, then the SBE plan cache is in
// use and we expect a pinned plan cache entry.
if (isSBEEnabled) {
    runTest({indexes: [{a: 1}], document: {_id: 0, a: "abc"}, filter: {a: "abc"}});
}

// Exercise the multi-planner using a case where there are multiple eligible indexes.
runTest({
    indexes: [{a: 1}, {b: 1}],
    document: {_id: 0, a: "abc", b: "123"},
    filter: {a: "abc", b: "123"},
});

// Test a rooted $or query. This should use the subplanner. The way that the subplanner interacts
// with the plan cache differs between the classic engine and SBE. In the classic engine, the plan
// for each branch is cached independently, whereas in SBE we cache the entire "composite" plan.
// This test is written to expect the SBE behavior, so it only runs when SBE is enabled.
if (isSBEEnabled) {
    runTest({
        indexes: [{a: 1}, {b: 1}, {c: 1}, {d: 1}],
        document: {_id: 0, a: "abc", b: "123", c: 4, d: 5},
        filter: {$or: [{a: "abc", b: "123"}, {c: 4, d: 5}]},
    });
}

st.stop();
