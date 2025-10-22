//
// Validate that join optimization does not run on sharded collections.
//
// @tags: [
//   requires_fcv_83,
// ]
//
import {getPlanStages} from "jstests/libs/query/analyze_plan.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function joinOptimizationRuns(db, baseColl, coll1, coll2) {
    // Simple pipeline normally eligible for join optimization.
    const pipeline = [
        {$lookup: {from: coll1, localField: "f1", foreignField: "f1", as: "l1"}},
        {$unwind: "$l1"},
        {$lookup: {from: coll2, localField: "f2", foreignField: "f2", as: "l2"}},
        {$unwind: "$l2"},
    ];

    const explain = db[baseColl].explain().aggregate(pipeline);
    jsTest.log.info({context: "Explain for pipeline", explain, output: db[baseColl].aggregate(pipeline).toArray()});
    return getPlanStages(explain, "NESTED_LOOP_JOIN_EMBEDDING").length > 0;
}

// Set up a sharded cluster.
const sharded = new ShardingTest({mongos: 1, shards: 2});

const db = sharded.getDB("test");
sharded.shard0.getDB("test").setLogLevel(5, "query");
sharded.shard1.getDB("test").setLogLevel(5, "query");
const docs = [{f1: "aaa", f2: 123}, {f1: "bbb", f2: 0}, {f2: -1}, {f1: "zzz"}];
assert.commandWorked(db["coll1"].insertMany(docs));
assert.commandWorked(db["coll2"].insertMany(docs));
assert.commandWorked(db["coll3"].insertMany(docs));
assert.commandWorked(db["coll4"].insertMany(docs));

// Ensure join optimization is disabled.
assert(sharded.shard0.getDB("test").adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
assert(sharded.shard1.getDB("test").adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
assert(!joinOptimizationRuns(db, "coll1", "coll2", "coll3"));

// Enable join optimization.
assert(sharded.shard0.getDB("test").adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
assert(sharded.shard1.getDB("test").adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
assert(joinOptimizationRuns(db, "coll1", "coll2", "coll3"));

// Enable sharding on the database. Should work.
assert(sharded.adminCommand({enableSharding: "test"}));
assert(joinOptimizationRuns(db, "coll1", "coll2", "coll3"));

// Shard a collection. Do not permit join optimization on sharded collections no matter where they appear in the pipeline.
assert.commandWorked(db["coll2"].createIndex({_id: "hashed"}));
assert(sharded.adminCommand({shardCollection: "test.coll2", key: {_id: "hashed"}}));
assert(!joinOptimizationRuns(db, "coll1", "coll2", "coll3"));
assert(!joinOptimizationRuns(db, "coll2", "coll1", "coll3"));
assert(!joinOptimizationRuns(db, "coll3", "coll2", "coll1"));

// Still works if we don't reference a sharded collection.
assert(joinOptimizationRuns(db, "coll3", "coll4", "coll1"));

// TODO SERVER-112725: why do we have to disable join opt here to terminate safely?
assert(sharded.shard0.getDB("test").adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
assert(sharded.shard1.getDB("test").adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));

sharded.stop();
