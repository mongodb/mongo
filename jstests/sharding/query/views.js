/**
 * Tests for expected behavior when querying a view that is based on a sharded collection.
 * @tags: [
 *  requires_fcv_63,
 * ]
 */
(function() {
"use strict";

// For profilerHasSingleMatchingEntryOrThrow.
load("jstests/libs/profiler.js");
// For checkSBEEnabled.
load("jstests/libs/sbe_util.js");
// For areAllCollectionsClustered.
load("jstests/libs/clustered_collections/clustered_collection_util.js");

// Legal values for the verifyExplainResult() 'optimizedAwayPipeline' argument.
const kOptFalse = 0;
const kOptTrue = 1;
const kOptEither = 2;

// Given sharded explain output in 'shardedExplain', verifies that the explain mode 'verbosity'
// affected the output verbosity appropriately, and that the response has the expected format.
// Set 'optimizedAwayPipeline' to:
//   kOptTrue if the pipeline is expected to be optimized away
//   kOptFalse if the pipeline is expected to be present
//   kOptEither if the call does not know so must accept either of the prior two cases
function verifyExplainResult(
    {shardedExplain = null, verbosity = "", optimizedAwayPipeline = kOptFalse} = {}) {
    assert.commandWorked(shardedExplain);
    assert(shardedExplain.hasOwnProperty("shards"), tojson(shardedExplain));
    for (let elem in shardedExplain.shards) {
        let shard = shardedExplain.shards[elem];
        let root;

        // Resolve 'kOptEither' to 'kOptTrue' or 'kOptFalse'. If 'shard' has a "queryPlanner"
        // property, this means the pipeline has been optimized away. (When the pipeline is present,
        // "queryPlanner" is instead a property of shard.stages[0].$cursor.)
        if (optimizedAwayPipeline == kOptEither) {
            if (shard.hasOwnProperty("queryPlanner")) {
                optimizedAwayPipeline = kOptTrue;
            } else {
                optimizedAwayPipeline = kOptFalse;
            }
        }

        // Verify the explain output.
        if (optimizedAwayPipeline == kOptTrue) {
            assert(shard.hasOwnProperty("queryPlanner"), tojson(shardedExplain));
            root = shard;
        } else if (optimizedAwayPipeline == kOptFalse) {
            assert(shard.stages[0].hasOwnProperty("$cursor"), tojson(shardedExplain));
            assert(shard.stages[0].$cursor.hasOwnProperty("queryPlanner"), tojson(shardedExplain));
            root = shard.stages[0].$cursor;
        } else {
            assert(false, `Unsupported 'optimizedAwayPipeline' value ${optimizedAwayPipeline}`);
        }
        if (verbosity === "queryPlanner") {
            assert(!root.hasOwnProperty("executionStats"), tojson(shardedExplain));
        } else if (verbosity === "executionStats") {
            assert(root.hasOwnProperty("executionStats"), tojson(shardedExplain));
            assert(!root.executionStats.hasOwnProperty("allPlansExecution"),
                   tojson("shardedExplain"));
        } else {
            assert.eq(verbosity, "allPlansExecution", tojson(shardedExplain));
            assert(root.hasOwnProperty("executionStats"), tojson(shardedExplain));
            assert(root.executionStats.hasOwnProperty("allPlansExecution"), tojson(shardedExplain));
        }
    }
}

let st = new ShardingTest({name: "views_sharded", shards: 2, other: {enableBalancer: false}});

let mongos = st.s;
let config = mongos.getDB("config");
let db = mongos.getDB(jsTestName());
db.dropDatabase();

const isClustered = ClusteredCollectionUtil.areAllCollectionsClustered(st.rs0.getPrimary());

let coll = db.getCollection("coll");

assert.commandWorked(config.adminCommand({enableSharding: db.getName()}));
st.ensurePrimaryShard(db.getName(), st.shard0.shardName);
assert.commandWorked(config.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));

assert.commandWorked(mongos.adminCommand({split: coll.getFullName(), middle: {a: 6}}));
assert.commandWorked(
    db.adminCommand({moveChunk: coll.getFullName(), find: {a: 25}, to: st.shard1.shardName}));

for (let i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({a: i}));
}

assert.commandWorked(db.createView("view", coll.getName(), [{$match: {a: {$gte: 4}}}]));
let view = db.getCollection("view");

const explainVerbosities = ["queryPlanner", "executionStats", "allPlansExecution"];

//
// find
//
assert.eq(5, view.find({a: {$lte: 8}}).itcount());

let result = db.runCommand({explain: {find: "view", filter: {a: {$lte: 7}}}});
verifyExplainResult(
    {shardedExplain: result, verbosity: "allPlansExecution", optimizedAwayPipeline: kOptTrue});
for (let verbosity of explainVerbosities) {
    result = db.runCommand({explain: {find: "view", filter: {a: {$lte: 7}}}, verbosity: verbosity});
    verifyExplainResult(
        {shardedExplain: result, verbosity: verbosity, optimizedAwayPipeline: kOptTrue});
}

//
// aggregate
//
assert.eq(5, view.aggregate([{$match: {a: {$lte: 8}}}]).itcount());

// Test that the explain:true flag for the aggregate command results in queryPlanner verbosity.
result = db.runCommand({aggregate: "view", pipeline: [{$match: {a: {$lte: 8}}}], explain: true});
verifyExplainResult(
    {shardedExplain: result, verbosity: "queryPlanner", optimizedAwayPipeline: kOptTrue});

result =
    db.runCommand({explain: {aggregate: "view", pipeline: [{$match: {a: {$lte: 8}}}], cursor: {}}});
verifyExplainResult(
    {shardedExplain: result, verbosity: "allPlansExecution", optimizedAwayPipeline: kOptTrue});
for (let verbosity of explainVerbosities) {
    result = db.runCommand({
        explain: {aggregate: "view", pipeline: [{$match: {a: {$lte: 8}}}], cursor: {}},
        verbosity: verbosity
    });
    verifyExplainResult(
        {shardedExplain: result, verbosity: verbosity, optimizedAwayPipeline: kOptTrue});
}

//
// count
//
assert.eq(5, view.count({a: {$lte: 8}}));

// "count" on a view that is a $match will produce different explain output on Classic vs SBE, as
// the query will be rewriten as a $group, but only SBE has a $group pushdown feature, which
// optimizes away the pipeline. Depending on build variant and engine selection flags, as well as
// specific configurations of individual nodes in multiversion clusters, we may get either the
// Classic or SBE explain variant, so here we accept either one ('kOptEither').
result = db.runCommand({explain: {count: "view", query: {a: {$lte: 8}}}});
verifyExplainResult(
    {shardedExplain: result, verbosity: "allPlansExecution", optimizedAwayPipeline: kOptEither});
for (let verbosity of explainVerbosities) {
    result = db.runCommand({explain: {count: "view", query: {a: {$lte: 8}}}, verbosity: verbosity});
    verifyExplainResult(
        {shardedExplain: result, verbosity: verbosity, optimizedAwayPipeline: kOptEither});
}

//
// distinct
//
result = db.runCommand({distinct: "view", key: "a", query: {a: {$lte: 8}}});
assert.commandWorked(result);
assert.eq([4, 5, 6, 7, 8], result.values.sort());

result = db.runCommand({explain: {distinct: "view", key: "a", query: {a: {$lte: 8}}}});
verifyExplainResult({shardedExplain: result, verbosity: "allPlansExecution"});
for (let verbosity of explainVerbosities) {
    result = db.runCommand(
        {explain: {distinct: "view", key: "a", query: {a: {$lte: 8}}}, verbosity: verbosity});
    verifyExplainResult({shardedExplain: result, verbosity: verbosity});
}

//
// Confirm cleanupOrphaned command fails.
//
result = st.getPrimaryShard(db.getName()).getDB("admin").runCommand({
    cleanupOrphaned: view.getFullName()
});
assert.commandFailedWithCode(result, ErrorCodes.CommandNotSupportedOnView);

//
//  Confirm getShardVersion command fails.
//
assert.commandFailedWithCode(db.adminCommand({getShardVersion: view.getFullName()}),
                             ErrorCodes.NamespaceNotSharded);

//
// Confirm that the comment parameter on a find command is retained when rewritten as an
// expanded aggregation on the view.
//
let sdb = st.shard0.getDB(jsTestName());
assert.commandWorked(sdb.setProfilingLevel(2));

assert.eq(5, view.find({a: {$lte: 8}}).comment("agg_comment").itcount());

profilerHasSingleMatchingEntryOrThrow({
    profileDB: sdb,
    filter: {
        "command.aggregate": coll.getName(),
        "command.comment": "agg_comment",
        "command.needsMerge": true,
        "command.pipeline.$mergeCursors": {$exists: false}
    }
});

st.stop();
})();
