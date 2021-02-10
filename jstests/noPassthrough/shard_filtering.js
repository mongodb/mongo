/**
 * This test is intended to exercise shard filtering logic. This test works by sharding a
 * collection, and then inserting orphaned documents directly into one of the shards. It then runs a
 * find() and makes sure that orphaned documents are filtered out.
 * @tags: [
 *   requires_sharding,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

// Deliberately inserts orphans outside of migration.
TestData.skipCheckOrphans = true;
const st = new ShardingTest({shards: 2});
const collName = "test.shardfilter";
const mongosDb = st.s.getDB("test");
const mongosColl = st.s.getCollection(collName);

assert.commandWorked(st.s.adminCommand({enableSharding: "test"}));
st.ensurePrimaryShard("test", st.shard1.name);
assert.commandWorked(
    st.s.adminCommand({shardCollection: collName, key: {a: 1, "b.c": 1, "d.e.f": 1}}));

// Put a chunk with no data onto shard0 in order to make sure that both shards get targeted.
assert.commandWorked(st.s.adminCommand({split: collName, middle: {a: 20, "b.c": 0, "d.e.f": 0}}));
assert.commandWorked(st.s.adminCommand({split: collName, middle: {a: 30, "b.c": 0, "d.e.f": 0}}));
assert.commandWorked(st.s.adminCommand(
    {moveChunk: collName, find: {a: 25, "b.c": 0, "d.e.f": 0}, to: st.shard0.shardName}));

// Shard the collection and insert some docs.
const docs = [
    {_id: 0, a: 1, b: {c: 1}, d: {e: {f: 1}}, g: 100},
    {_id: 1, a: 1, b: {c: 2}, d: {e: {f: 2}}, g: 100.9},
    {_id: 2, a: 1, b: {c: 3}, d: {e: {f: 3}}, g: "a"},
    {_id: 3, a: 1, b: {c: 3}, d: {e: {f: 3}}, g: [1, 2, 3]},
    {_id: 4, a: "a", b: {c: "b"}, d: {e: {f: "c"}}, g: null},
    {_id: 5, a: 1.0, b: {c: "b"}, d: {e: {f: Infinity}}, g: NaN}
];
assert.commandWorked(mongosColl.insert(docs));
assert.eq(mongosColl.find().itcount(), 6);

// Insert some documents with valid partial shard keys to both shards. The versions of these
// documents on shard0 are orphans, since all of the data is owned by shard1.
const docsWithMissingAndNullKeys = [
    {_id: 6, a: "missingParts"},
    {_id: 7, a: null, b: {c: 1}, d: {e: {f: 1}}},
    {_id: 8, a: "null", b: {c: null}, d: {e: {f: 1}}},
    {_id: 9, a: "deepNull", b: {c: 1}, d: {e: {f: null}}},
];
assert.commandWorked(st.shard0.getCollection(collName).insert(docsWithMissingAndNullKeys));
assert.commandWorked(st.shard1.getCollection(collName).insert(docsWithMissingAndNullKeys));

// Insert orphan docs without missing or null shard keys onto shard0 and test that they get filtered
// out.
const orphanDocs = [
    {_id: 10, a: 100, b: {c: 10}, d: {e: {f: 999}}, g: "a"},
    {_id: 11, a: 101, b: {c: 11}, d: {e: {f: 1000}}, g: "b"}
];
assert.commandWorked(st.shard0.getCollection(collName).insert(orphanDocs));
assert.eq(mongosColl.find().itcount(), 10);

// Insert docs directly into shard0 to test that regular (non-null, non-missing) shard keys get
// filtered out.
assert.commandWorked(st.shard0.getCollection(collName).insert(docs));
assert.eq(mongosColl.find().itcount(), 10);

// Ensure that shard filtering works correctly for a query that can use the index supporting the
// shard key. In this case, shard filtering can occur before the FETCH stage, but the plan is not
// covered.
let explain = mongosColl.find({a: {$gte: 0}}).explain();
assert.eq(explain.queryPlanner.winningPlan.stage, "SHARD_MERGE", explain);
assert(planHasStage(mongosDb, explain.queryPlanner.winningPlan, "SHARDING_FILTER"), explain);
assert(isIxscan(mongosDb, explain.queryPlanner.winningPlan), explain);
assert(!isIndexOnly(mongosDb, explain.queryPlanner.winningPlan), explain);
assert.sameMembers(mongosColl.find({a: {$gte: 0}}).toArray(), [
    {_id: 0, a: 1, b: {c: 1}, d: {e: {f: 1}}, g: 100},
    {_id: 1, a: 1, b: {c: 2}, d: {e: {f: 2}}, g: 100.9},
    {_id: 2, a: 1, b: {c: 3}, d: {e: {f: 3}}, g: "a"},
    {_id: 3, a: 1, b: {c: 3}, d: {e: {f: 3}}, g: [1, 2, 3]},
    {_id: 5, a: 1, b: {c: "b"}, d: {e: {f: Infinity}}, g: NaN}
]);

// In this case, shard filtering is done as part of a covered plan.
explain = mongosColl.find({a: {$gte: 0}}, {_id: 0, a: 1}).explain();
assert.eq(explain.queryPlanner.winningPlan.stage, "SHARD_MERGE", explain);
assert(planHasStage(mongosDb, explain.queryPlanner.winningPlan, "SHARDING_FILTER"), explain);
assert(isIxscan(mongosDb, explain.queryPlanner.winningPlan), explain);
assert(isIndexOnly(mongosDb, explain.queryPlanner.winningPlan), explain);
assert.sameMembers(mongosColl.find({a: {$gte: 0}}, {_id: 0, a: 1}).toArray(), [
    {a: 1},
    {a: 1},
    {a: 1},
    {a: 1},
    {a: 1},
]);

// Drop the collection and shard it by a new key that has no dotted fields. Again, make sure that
// shard0 has an empty chunk.
assert(mongosColl.drop());
assert.commandWorked(st.s.adminCommand({shardCollection: collName, key: {a: 1, b: 1, c: 1, d: 1}}));
assert.commandWorked(st.s.adminCommand({split: collName, middle: {a: 20, b: 0, c: 0, d: 0}}));
assert.commandWorked(st.s.adminCommand({split: collName, middle: {a: 30, b: 0, c: 0, d: 0}}));
assert.commandWorked(st.s.adminCommand(
    {moveChunk: collName, find: {a: 25, b: 0, c: 0, d: 0}, to: st.shard0.shardName}));

// Insert some data via mongos, and also insert some documents directly to shard0 to produce an
// orphans.
assert.commandWorked(mongosColl.insert([
    {_id: 0, a: 0, b: 0, c: 0, d: 0},
    {_id: 1, a: 1, b: 1, c: 1, d: 1},
    {_id: 2, a: -1, b: -1, c: -1, d: -1},
]));
assert.commandWorked(st.shard0.getCollection(collName).insert({_id: 3, a: 0, b: 0, c: 0, d: 0}));
assert.commandWorked(st.shard0.getCollection(collName).insert({_id: 4, a: 0, b: 99, c: 0, d: 99}));
assert.commandWorked(st.shard0.getCollection(collName).insert({_id: 5, a: 0, b: 0, c: 99, d: 99}));
assert.commandWorked(st.shard0.getCollection(collName).insert({_id: 6, a: 0, b: 99, c: 99, d: 99}));

// Run a query that can use covered shard filtering where the projection involves more than one
// field of the shard key.
explain = mongosColl.find({a: {$gte: 0}}, {_id: 0, a: 1, b: 1, d: 1}).explain();
assert.eq(explain.queryPlanner.winningPlan.stage, "SHARD_MERGE", explain);
assert(planHasStage(mongosDb, explain.queryPlanner.winningPlan, "SHARDING_FILTER"), explain);
assert(isIxscan(mongosDb, explain.queryPlanner.winningPlan), explain);
assert(isIndexOnly(mongosDb, explain.queryPlanner.winningPlan), explain);
assert.sameMembers(mongosColl.find({a: {$gte: 0}}, {_id: 0, a: 1, b: 1, d: 1}).toArray(),
                   [{a: 0, b: 0, d: 0}, {a: 1, b: 1, d: 1}]);

// Run a query that will use a covered OR plan.
explain = mongosColl.find({$or: [{a: 0, c: 0}, {a: 25, c: 0}]}, {_id: 0, a: 1, c: 1}).explain();
assert.eq(explain.queryPlanner.winningPlan.stage, "SHARD_MERGE", explain);
assert(planHasStage(mongosDb, explain.queryPlanner.winningPlan, "SHARDING_FILTER"), explain);
assert(planHasStage(mongosDb, explain.queryPlanner.winningPlan, "OR"), explain);
assert(isIndexOnly(mongosDb, explain.queryPlanner.winningPlan), explain);
assert.sameMembers(
    mongosColl.find({$or: [{a: 0, c: 0}, {a: 25, c: 0}]}, {_id: 0, a: 1, c: 1}).toArray(),
    [{a: 0, c: 0}]);

// Similar case to above, but here the index scans involve a single interval of the index.
explain = mongosColl.find({$or: [{a: 0, b: 0}, {a: 25, b: 0}]}, {_id: 0, a: 1, b: 1}).explain();
assert.eq(explain.queryPlanner.winningPlan.stage, "SHARD_MERGE", explain);
assert(planHasStage(mongosDb, explain.queryPlanner.winningPlan, "SHARDING_FILTER"), explain);
assert(planHasStage(mongosDb, explain.queryPlanner.winningPlan, "OR"), explain);
assert(isIndexOnly(mongosDb, explain.queryPlanner.winningPlan), explain);
assert.sameMembers(
    mongosColl.find({$or: [{a: 0, b: 0}, {a: 25, b: 0}]}, {_id: 0, a: 1, b: 1}).toArray(),
    [{a: 0, b: 0}]);

st.stop();
})();
