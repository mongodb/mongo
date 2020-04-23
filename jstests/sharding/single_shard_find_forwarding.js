/**
 * Tests that Collection.find().skip(n).limit(m), on an single shard collection applies limit and
 * skip on mongod, rather than fetching all documents satisfying the predicate passed to find and
 * applying limit and skip on mongos. This is intended to test the optimzation in SERVER-36290.
 *
 * The test works by creating an single shard collection, and then inserting documents directly into
 * the primary shard. It then runs a find().skip(n).limit(m) and ensures that the document count
 * meets expectation. It then runs the same test against a sharded collection with a single shard.
 */
// @tags: [
//   requires_find_command,
// ]
load("jstests/libs/profiler.js");      // For profilerHas*OrThrow helper functions.
load("jstests/libs/analyze_plan.js");  // For getPlanStages helper function.

(function() {
"use strict";

function testArraySorted(arr, key) {
    for (let i = 0; i < arr.length - 1; i++) {
        assert(arr[i][key].valueOf() <= arr[i + 1][key].valueOf());
    }
}

const testName = "single_shard_find_forwarding";

const st = new ShardingTest({shards: 2});
const testDB = st.s.getDB(testName);
const shardedColl = testDB.coll;
const singleShardColl = testDB.singleShard;
const shard0DB = st.shard0.getDB(testName);
const shard1DB = st.shard1.getDB(testName);

assert.commandWorked(st.s0.adminCommand({enableSharding: testDB.getName()}));
st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);

// Shard shardedColl using hashed sharding
st.shardColl(shardedColl, {_id: "hashed"}, false);

let nDocs = 3;
const shardedDocs = [
    {x: 0, _id: 0},
    {x: 2, _id: 2},
    {x: 1, _id: 1},
];
assert.commandWorked(shardedColl.insert(shardedDocs));

const docs = [
    {x: 4, _id: 4},
    {x: 3, _id: 3},
    {x: 5, _id: 5},
];
assert.commandWorked(singleShardColl.insert(docs));

// Enable profiler log to check if skip and limit are passed and executed on mongod for the
// unsharded collection.
assert.commandWorked(testDB.adminCommand({profile: 0, slowms: -1}));

// Capture all commands in the profile log. To do this, enable profiling and changes the 'slowms'
// threshold to -1ms.
shard0DB.setProfilingLevel(0);
shard0DB.system.profile.drop();
shard0DB.setProfilingLevel(2);

let shardedCursor = shardedColl.find();
let singleShardCursor = singleShardColl.find().skip(1).limit(nDocs - 1).comment(testName);
assert.eq(shardedCursor.itcount(), nDocs);
assert.eq(singleShardCursor.itcount(), nDocs - 1);

// Query profiler on the singleShardColl shardDB and check if limit and skip get forwarded.
profilerHasSingleMatchingEntryOrThrow({
    profileDB: shard0DB,
    filter: {"command.skip": 1, "command.limit": nDocs - 1, "command.comment": testName}
});

// Skip past all of the documents
assert.eq(singleShardColl.find().skip(4).itcount(), 0);
assert.eq(singleShardColl.find().skip(4).limit(nDocs).itcount(), 0);

// Since we are not applying sortKey on mongos, check sorting occurs on mongod and not on mongos.
let sorted = singleShardColl.find().sort({x: 1}).toArray();
testArraySorted(sorted, "x");
let plan = singleShardColl.explain().find().sort({x: 1});
let sorts = getPlanStages(plan, "SORT");
assert(sorts.length == 0);

// Check that we didn't break sorting on mongos.
let sortedS = shardedColl.find().sort({x: 1}).toArray();
testArraySorted(sortedS, "x");

// Insert a larger set of docs into the collection and see if skip and limit work.
const singleShardColl2 = testDB.unsharded2;
nDocs = 1000;
let bulk = singleShardColl2.initializeUnorderedBulkOp();
for (let i = 0; i < nDocs; i++) {
    bulk.insert({x: i, _id: i});
}
assert.commandWorked(bulk.execute());

assert.eq(singleShardColl2.find().skip(1).limit(nDocs).itcount(), nDocs - 1);
assert.eq(singleShardColl2.find().skip(nDocs / 2).limit(nDocs).itcount(), nDocs / 2);
assert.eq(singleShardColl2.find().skip(nDocs - 1).limit(nDocs).itcount(), 1);
assert.eq(singleShardColl2.find().skip(nDocs + 1000).limit(nDocs).itcount(), 0);

st.stop();
})();
